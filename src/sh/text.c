#include "../compiler_memory.c"

/* sort has its command entry point later in the file.  ptx consumes its same
   byte comparator without moving or cloning the ordering engine. */
static PURE bipolar sort_compare_bytes(p8 address_to one,
                                       positive one_length,
                                       p8 address_to two,
                                       positive two_length,
                                       positive how);

/*
        The text utilities, as one body of code.

        grep, sed, cut, tr, sort, uniq, head, tail, wc, tee, rev, nl and fold
        are thirteen programs and one file, because eleven of them are the
        same program with a different inner loop: read lines from a list of
        files or from standard input, decide something about each one, write
        bytes out. Writing that shape thirteen times is how thirteen slightly
        different bugs get written down.

        Record storage is a fixed array or a slice of one arena taken once
        from the kernel. Operand indexes grow in a separate checked mapping,
        because a process may legally name many small files. A record longer
        than TEXT_LINE_MAX is refused instead of being handed to a utility
        with its tail silently missing: a bounded implementation may have a
        ceiling, but plausible truncated output is not a valid answer.

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
static bool text_out_failed;

static fn text_flush()
{
        if (!buffered_flush(text_out_handle, text_out_buffer,
                            address_of text_out_used))
                text_out_failed = true;
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
        if (!buffered_write_deferred_equal(text_out_handle, text_out_buffer,
                                           TEXT_OUT_MAX, address_of text_out_used,
                                           data, length))
                text_out_failed = true;
}

// Callers use this only for a span proven to fit TEXT_OUT_MAX and fill every
// byte before making another output call.
static p8 address_to text_reserve(positive length)
{
        p8 address_to at = buffered_reserve(
            text_out_handle, text_out_buffer, TEXT_OUT_MAX,
            address_of text_out_used, length);

        if (!at)
                text_out_failed = true;

        return at;
}

static fn text_put_character(p8 character)
{
        if (!buffered_write_byte(text_out_handle, text_out_buffer, TEXT_OUT_MAX,
                                 address_of text_out_used, character))
                text_out_failed = true;
}

static inline INLINE fn text_put_string(string_address value)
{
        text_put(value, (positive)__builtin_strlen((const char address_to)value));
}

/*
        A byte written so a terminal can show it, in the spelling cat -v
        settled and cmp -b copied: the high half as M- and then the same rule
        again on what is left, 127 as ^?, a control character as ^ and the
        letter sixty four above it, and anything else as itself.

        Into a caller's array rather than straight out, because cmp needs the
        two bytes it is comparing side by side in a line it has not finished
        building. Terminated as well as measured, since cmp prints the result
        as a string. Four bytes is the widest answer -- M-^? -- so five is
        room enough for any of them.
*/
#define TEXT_VISIBLE_MAX 5

static positive text_visible(p8 address_to into, p8 value)
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
                into[have++] = value;

        into[have] = end;
        return have;
}

static fn text_error_raw(string_address text)
{
        system_write_all(2, text, string_length(text));
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

        if (text_out_failed)
        {
                if (string_equals(text_name, "grep") ||
                    string_equals(text_name, "sort"))
                        return 2;

                if (string_equals(text_name, "sed"))
                        return 4;

                return 1;
        }

        return code;
}

/*
        A complaint and the status that goes with it, which is how nearly
        every refusal in this file ends.

        The status is the caller's because the tools do not agree on one:
        most answer 1, grep and sort answer 2, and sed answers 4. Saying both
        halves in one line is also what keeps them together -- a refusal that
        prints and then falls through to the ordinary exit is the bug this
        shape cannot have.
*/
static b32 text_refuse(string_address about, string_address reason, b32 code)
{
        text_error(about, reason);

        return text_done(code);
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
        /* Keep both the alignment addition and the remaining-room test from
           wrapping. Most callers are bounded before they arrive here, but
           this is the allocator boundary: a missed product check must fail
           here rather than turn a large request into a small pointer inside
           the arena and let the caller write past it. */
        if (bytes > TEXT_ARENA_BYTES || bytes > positive_max - 15)
        {
                text_error(null, "input too large");
                return null;
        }

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

        if (text_arena_used > TEXT_ARENA_BYTES ||
            bytes > TEXT_ARENA_BYTES - text_arena_used)
        {
                text_error(null, "input too large");
                return null;
        }

        address_any at = text_arena + text_arena_used;
        text_arena_used += bytes;
        return at;
}

/* Read an unbounded descriptor into the newest arena object. Rewinding before
   each doubling keeps its address stable and makes growth a capacity change,
   not an allocate-and-copy loop. The final rewind gives unused capacity back
   before the caller retains anything else. */
static p8 address_to text_arena_read_all(positive handle, positive first,
                                         positive address_to length,
                                         bool address_to read_failed)
{
        positive mark = text_arena_used;
        positive room = first;
        positive used = 0;
        p8 address_to bytes = (p8 address_to)text_arena_take(room);

        address_to length = 0;
        if (read_failed)
                address_to read_failed = false;
        if (!bytes)
                return null;

        while (true)
        {
                if (used == room)
                {
                        positive larger = room < positive_max
                            ? memory_growth(room, room + 1, first)
                            : 0;
                        positive available = TEXT_ARENA_BYTES - mark;

                        if (larger > available)
                                larger = room < available ? available : 0;

                        if (!larger)
                                goto failed;

                        text_arena_used = mark;
                        bytes = (p8 address_to)text_arena_take(larger);

                        if (!bytes)
                                goto failed;

                        room = larger;
                }

                bipolar got = system_read_retry(handle, bytes + used,
                                                room - used);

                if (got < 0)
                {
                        if (read_failed)
                                address_to read_failed = true;
                        goto failed;
                }
                if (!got)
                        break;

                used += (positive)got;
        }

        bytes[used] = end;
        text_arena_used = mark + ((used + 1 + 15) & ~(positive)15);
        address_to length = used;
        return bytes;

failed:
        text_arena_used = mark;
        return null;
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
        return system_open_at_mode(AT_FDCWD,
                             path, flags, mode);
}

typedef struct
{
        positive handle;
        positive filled;
        positive position;
        bool finished;
        bool opened;
        // Whether the reader stopped because the input failed rather than
        // because it ended. The line tools turn this into an exit status the
        // moment it happens; cmp keeps its own answer and asks later.
        bool failed;
        string_address name;
        p8 buffer[TEXT_READ_MAX];
} text_reader;

static text_reader text_input;
/* pr -r is the one line tool which deliberately suppresses an open
   diagnostic.  Keep that policy at the shared reader boundary so its merge
   cursors do not grow a second open path. */
static bool text_quiet_open;
/* One sentinel slot is used while a sed script file is turned into text. */
static p8 text_line[TEXT_LINE_MAX + 1];
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

/*
        Opening one, which every tool here does the same way: no name at all
        or a bare - is standard input, which is left open afterwards because
        the process did not open it. Anything else is a path.

        The reader is emptied before the open is attempted, so a failure
        cannot leave the bytes of the last file sitting behind it.
*/
static bool text_reader_open(text_reader address_to reader, string_address path)
{
        reader->filled = 0;
        reader->position = 0;
        reader->finished = false;
        reader->failed = false;
        reader->opened = false;
        reader->name = path;

        if (!path || (path[0] == '-' && path[1] == '\0'))
        {
                reader->handle = 0;
                return true;
        }

        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
        {
                /* Every reason an input can refuse to open is worth the
                   name the system gives it; assuming the absent one made a
                   denied file and a symlink loop read alike. */
                if (!text_quiet_open)
                        text_error(path, file_reason(handle));
                reader->failed = true;
                return false;
        }

        reader->handle = (positive)handle;
        reader->opened = true;
        return true;
}

static inline INLINE fn text_close_handle(bool address_to opened,
                                          positive handle)
{
        if (address_to opened)
                system_close(handle);

        address_to opened = false;
}

/*
        More bytes, or false at the end of the input and false again when the
        read itself failed. The two are told apart by `failed` rather than by
        the answer, because a tool that is only moving bytes does not need to
        know which of them happened.
*/
static bool text_reader_fill_amount(text_reader address_to reader,
                                    positive amount)
{
        if (reader->position < reader->filled)
                return true;

        if (reader->finished)
                return false;

        bipolar got = system_read_retry(reader->handle, reader->buffer,
                                        min(amount, (positive)TEXT_READ_MAX));

        if (got <= 0)
        {
                reader->finished = true;

                if (got < 0)
                {
                        text_error(reader->name, "Read error");
                        reader->failed = true;
                }

                return false;
        }

        reader->filled = (positive)got;
        reader->position = 0;
        return true;
}

static inline INLINE bool text_reader_fill(text_reader address_to reader)
{
        return text_reader_fill_amount(reader, TEXT_READ_MAX);
}

/*
        The one input the line tools share, and the exit status a failure on
        it leaves behind. cmp reads its two sides through the reader above
        and keeps its own answer, which is why the status lives here and not
        in the reader.
*/
static bool text_open(string_address path)
{
        if (text_reader_open(address_of text_input, path))
                return true;

        text_status = text_status ? text_status : 1;
        return false;
}

static fn text_close()
{
        text_close_handle(address_of text_input.opened, text_input.handle);
}

static bool text_fill()
{
        if (text_reader_fill(address_of text_input))
                return true;

        // Set on every later call as well as the one that failed, which
        // cannot change it: the first failure already made it one.
        if (text_input.failed)
                text_status = text_status ? text_status : 1;

        return false;
}

static bool text_fill_amount(positive amount)
{
        if (text_reader_fill_amount(address_of text_input, amount))
                return true;

        if (text_input.failed)
                text_status = text_status ? text_status : 1;

        return false;
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

                if (take > room)
                {
                        /*
                                Stop this input here. Merely returning false
                                while bytes remain in the reader would let a
                                nested sed N, or another caller that asks
                                again, treat the dropped tail as a new line.
                        */
                        text_input.position = text_input.filled;
                        text_input.finished = true;
                        text_line_length = 0;
                        text_line_ended = false;
                        text_error(null, "line too long");
                        text_status = text_status ? text_status : 1;
                        return false;
                }

                memory_copy(text_line + text_line_length, at, take);
                text_line_length += take;
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

/*
        A complete record already in the reader is immutable until the caller
        asks for another refill, so line-oriented streaming tools can inspect
        it in place. Only a record split across reads needs text_line's owned
        storage. This preserves the ordinary line reader as the bounded edge
        path while removing one full copy from the common path.
*/
static fn text_line_preserve(p8 address_to address_to previous,
                             positive previous_length,
                             p8 address_to storage)
{
        if (previous && address_to previous && address_to previous != storage)
        {
                // The terminator is part of the view contract. Keeping it
                // with the bytes also makes later output one buffered write.
                memory_copy(storage, address_to previous, previous_length + 1);
                address_to previous = storage;
        }
}

static bool text_line_view(p8 address_to address_to line,
                           positive address_to length,
                           p8 address_to address_to previous,
                           positive previous_length,
                           p8 address_to previous_storage)
{
        if (text_input.position >= text_input.filled)
                text_line_preserve(previous, previous_length,
                                   previous_storage);

        if (!text_fill())
                return false;

        p8 address_to at = text_input.buffer + text_input.position;
        positive left = text_input.filled - text_input.position;
        p8 address_to found = memory_first_of(at, text_delimiter, left);

        if (found)
        {
                address_to line = at;
                address_to length = (positive)(found - at);
                text_input.position += address_to length + 1;
                text_line_ended = true;
                return true;
        }

        // text_line_next refills the reader and reuses text_line. Preserve a
        // prior view from either store before it can be overwritten.
        text_line_preserve(previous, previous_length, previous_storage);

        if (!text_line_next())
                return false;

        text_line[text_line_length] = text_delimiter;
        address_to line = text_line;
        address_to length = text_line_length;
        return true;
}

static fn text_put_line()
{
        text_put(text_line, text_line_length);

        if (text_line_ended)
                text_put_character(text_delimiter);
}

// Whatever is left of the input, straight out, without looking at any of it.
// This is what cat is doing nearly every time it is run, and what head and
// tail do once they have seeked to where their answer starts.
static fn text_put_rest()
{
        while (text_fill())
        {
                text_put(text_input.buffer + text_input.position,
                         text_input.filled - text_input.position);
                text_input.position = text_input.filled;
        }
}

/*
        byte_is_blank's complement, for string_span_max.

        Not the library's own string_set_not_blanks, which leaves the
        terminator out so that a run has somewhere to stop when the caller
        gives no bound. Every line here is a length rather than a string and
        may hold a NUL that is neither a blank nor an end, so the bound is the
        only thing allowed to stop it.
*/
static b8 text_set_inside[STRING_SET_BYTES];

static const b8 address_to text_inside()
{
        if (!text_set_inside[0])
        {
                memory_fill(text_set_inside, 1, sizeof(text_set_inside));
                text_set_inside[' '] = 0;
                text_set_inside['\t'] = 0;
        }

        return text_set_inside;
}

/*
        Coreutils' numeric options accept leading white space and a plus, but
        not trailing bytes. string_digits_exact is the floor for the common
        plain spelling; this cold option parser adds the range contract it
        deliberately does not carry. A saturating caller is useful for cmp:
        a count larger than the address space means through the end, never
        zero bytes after an unsigned wrap.
*/
static bool text_unsigned_option(string_address source, bool saturate,
                                 positive address_to value)
{
        positive at = 0;
        positive made = 0;
        positive ceiling = (positive)-1;
        bool overflow = false;
        positive digits = 0;

        if (!source)
                return false;

        while (byte_is_space(source[at]))
                at++;

        if (source[at] == '+')
                at++;
        else if (source[at] == '-')
                return false;

        while (byte_is_digit(source[at]))
        {
                positive digit = source[at++] - '0';

                digits++;

                if (made > (ceiling - digit) / 10)
                {
                        overflow = true;
                        made = ceiling;
                }
                else if (!overflow)
                        made = made * 10 + digit;
        }

        if (!digits || source[at] || (overflow && !saturate))
                return false;

        address_to value = made;
        return true;
}

static bool text_word(p8 character)
{
        return byte_is_alnum(character) || character == '_';
}

/*
        Arguments.

        program_argument counts the program's own name as the first, so
        everything below starts at one and text_argument_count is what stops
        the walk.
*/
static b32 text_argument_count;
static positive text_files_count;
static bool text_files_failed;

static fn text_begin(string_address name)
{
        /* A shell may run several built-in tools in one process. */
        text_out_used = 0;
        text_out_handle = 1;
        text_out_failed = false;
        text_status = 0;
        text_name = name;
        text_argument_count = program_argument_count();
        text_files_count = 0;
        text_files_failed = false;
        text_quiet_open = false;
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

static b32 regex_set_count;
static bool regex_escapes;
static bool regex_broken;

enum
{
        REGEX_DOT_NEWLINE = 1,
        REGEX_LINE_ANCHORS = 2,
        REGEX_BASIC_REPEATS = 4,
        REGEX_POLICY_DEFAULT = REGEX_DOT_NEWLINE | REGEX_BASIC_REPEATS,
        REGEX_POLICY_TAC = REGEX_LINE_ANCHORS,
};

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

/*
        The pattern that is not a pattern.

        Most of what grep and sed are asked for is a fixed string, and the
        machine above answers it one byte at a time through a first-byte table
        -- thirteen million table lookups on a thirteen megabyte file. A
        program that is nothing but a run of characters is that run, and
        memory_search finds it thirty two positions at a time. Compiled once,
        here, so every caller of regex_search gets it.

        Longer than this and the machine takes it back, which costs speed on a
        pattern nobody writes and no correctness anywhere.
*/
#define REGEX_LITERAL_MAX 256

// Two tables in one row: the bytes a match can begin with, and behind them
// their complement as a set for the span routine, which is how the skip to
// the next possible start runs over a line eight or more bytes at a time
// rather than one. A program's row moves with the program.
static p8 regex_first_store[REGEX_FIRST_MAX][512];
static p8 regex_last_store[REGEX_FIRST_MAX][256];
static p8 regex_literal_store[REGEX_FIRST_MAX][REGEX_LITERAL_MAX];
static b32 regex_first_used;
static positive regex_stop_wanted = TEXT_UNSET;
static b32 regex_loop_list[REGEX_LOOPS_KEPT];
static p8 regex_visited[REGEX_CODE_MAX];

/* One shape is both the live VM selection and the persistent part of a saved
   program.  Capture/select therefore cannot drift when another derived field
   is added, while the uncommon loop vector retains its conditional copy. */
typedef struct
{
        regex_instruction address_to code;
        p8(address_to sets)[32];
        p8 address_to first;
        p8 address_to last;
        p8 address_to literal;
        positive literal_length;
        positive2 literal_anchors;
        b32 length;
        b32 groups;
        bool extended;
        bool icase;
        p8 policy;
        bool first_known;
        bool last_known;
        bool anchored;
        bool alternates;
        positive slot_used;
        b32 loop_count;
} regex_state;

typedef struct
{
        regex_state state;
        b32 loops[REGEX_LOOPS_KEPT];
} regex_program;

static regex_state regex_context = {
    .code = regex_store,
    .sets = regex_set_store,
    .first = regex_first_store[0],
    .last = regex_last_store[0],
    .literal = regex_literal_store[0],
    .policy = REGEX_POLICY_DEFAULT,
    .slot_used = REGEX_SLOT_MAX,
};

#define regex_code regex_context.code
#define regex_sets regex_context.sets
#define regex_first regex_context.first
#define regex_last regex_context.last
#define regex_literal regex_context.literal
#define regex_literal_length regex_context.literal_length
#define regex_literal_anchors regex_context.literal_anchors
#define regex_length_code regex_context.length
#define regex_group_count regex_context.groups
#define regex_extended regex_context.extended
#define regex_icase regex_context.icase
#define regex_policy regex_context.policy
#define regex_first_known regex_context.first_known
#define regex_last_known regex_context.last_known
#define regex_anchored regex_context.anchored
#define regex_alternates regex_context.alternates
#define regex_slot_used regex_context.slot_used
#define regex_loop_count regex_context.loop_count

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

        memory_copy(regex_code + where + count, regex_code + where,
                    (positive)(regex_length_code - where) *
                        sizeof(regex_instruction));

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
                        positive used;
                        b32 class = byte_class_parse(
                            regex_pattern + regex_pattern_at,
                            regex_pattern_length - regex_pattern_at,
                            address_of used);

                        if (class >= 0)
                        {
                                for (b32 c = 0; c < 256; c++)
                                        if (byte_class_holds(class, (p8)c))
                                                regex_set_add_folded(which,
                                                                     (p8)c);
                                regex_pattern_at += used;
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
                bool wanted = space ? byte_is_space((p8)c) : text_word((p8)c);

                if (wanted != negate)
                        regex_set_add(set, (p8)c);
        }

        b32 at = regex_emit(REGEX_SET);

        regex_code[at].set = set;
}

static fn regex_emit_literal(p8 character)
{
        b32 at = regex_emit(REGEX_CHAR);

        regex_code[at].value = regex_icase ? (p8)byte_to_lower(character) : character;
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

static fn regex_parse_group(bool escaped)
{
        bool capture = regex_group_count < REGEX_GROUP_MAX;
        b32 group = capture ? ++regex_group_count : 0;

        if (capture)
        {
                b32 open = regex_emit(REGEX_SAVE);
                regex_code[open].value = (p8)(group * 2);
        }

        regex_parse_alternation();

        if (escaped ? regex_peek() == '\\' && regex_peek_at(1) == ')'
                    : regex_peek() == ')')
                regex_pattern_at += escaped ? 2 : 1;
        else
                regex_broken = true;

        if (capture)
        {
                b32 shut = regex_emit(REGEX_SAVE);
                regex_code[shut].value = (p8)(group * 2 + 1);
        }
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
                regex_parse_group(false);

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
                        regex_parse_group(true);

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

        if (at >= regex_pattern_length)
                return false;

        positive taken;
        b32 first = (b32)string_digits_max(regex_pattern + at,
                                            regex_pattern_length - at,
                                            address_of taken);
        b32 second = -1;

        // A missing lower bound is zero, {,3} being {0,3} to glibc.
        if (!taken && !(at < regex_pattern_length && regex_pattern[at] == ','))
                return false;

        at += taken;

        if (at < regex_pattern_length && regex_pattern[at] == ',')
        {
                at++;

                positive value = string_digits_max(regex_pattern + at,
                                                    regex_pattern_length - at,
                                                    address_of taken);

                if (taken)
                {
                        second = (b32)value;
                        at += taken;
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

                // In a basic expression a star with only an anchor in front
                // of it is a plain star: ^* matches a line beginning with
                // one, as it does for the reference grep, and repeating the
                // anchor matched every line.
                if (character == '*' && !regex_extended &&
                    regex_code[start].code == REGEX_BOL)
                {
                        regex_pattern_at++;
                        start = regex_length_code;

                        b32 at = regex_emit(REGEX_CHAR);

                        regex_code[at].value = '*';
                        continue;
                }

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
                else if ((regex_policy & REGEX_BASIC_REPEATS) &&
                         !regex_extended &&
                         character == '\\' &&
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
                         ((regex_policy & REGEX_BASIC_REPEATS) &&
                          !regex_extended &&
                          character == '\\' && regex_peek_at(1) == '{'))
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
                else
                        regex_broken = true;

                regex_parse_branch();
        }

        for (b32 i = 0; i < jump_count; i++)
                regex_code[jumps[i]].x = regex_length_code;

        return start;
}

static fn regex_select(regex_program address_to which)
{
        regex_context = which->state;

        if (regex_loop_count > 0)
                memory_copy_apart(
                    regex_loop_list, which->loops,
                    (positive)(regex_loop_count < REGEX_LOOPS_KEPT
                                   ? regex_loop_count
                                   : REGEX_LOOPS_KEPT) *
                        sizeof(b32));
}

/* Snapshot the selected VM program without changing pool ownership.  The
   shell conditional engine uses this transiently; cached grep, sed and AWK
   programs commit the same snapshot with regex_keep below. */
static fn regex_capture(regex_program address_to which)
{
        which->state = regex_context;

        if (regex_loop_count > 0)
                memory_copy_apart(
                    which->loops, regex_loop_list,
                    (positive)(regex_loop_count < REGEX_LOOPS_KEPT
                                   ? regex_loop_count
                                   : REGEX_LOOPS_KEPT) *
                        sizeof(b32));

}

// Takes what was just compiled out of the pool's free space and hands back a
// handle to it, so the next compile starts after it rather than over it.
static fn regex_keep(regex_program address_to which)
{
        regex_capture(which);
        regex_pool_used += regex_length_code;
        regex_pool_sets += regex_set_count;
}

/* Add one consuming VM instruction to a possible first/last-byte table. */
static inline INLINE fn regex_edge_add(p8 address_to table, p8 kind,
                                       p8 value, b32 set)
{
        if (kind == REGEX_ANY)
        {
                memory_fill(table, 1, 256);

                if (!(regex_policy & REGEX_DOT_NEWLINE))
                        table['\n'] = 0;

                return;
        }

        if (kind == REGEX_SET)
        {
                for (b32 c = 0; c < 256; c++)
                        if (regex_set_has(set, (p8)c))
                                table[c] = 1;

                return;
        }

        table[value] = 1;

        if (!regex_icase)
                return;

        if (value >= 'a' && value <= 'z')
                table[value - 32] = 1;

        if (value >= 'A' && value <= 'Z')
                table[value + 32] = 1;
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
                regex_edge_add(regex_first, REGEX_CHAR, inst->value, 0);
                return false;

        case REGEX_ANY:
                regex_edge_add(regex_first, REGEX_ANY, 0, 0);
                return false;

        case REGEX_SET:
                regex_edge_add(regex_first, REGEX_SET, 0, inst->set);
                return false;

        case REGEX_REPEAT:
                regex_edge_add(regex_first, inst->kind, inst->value,
                               inst->set);
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

                regex_edge_add(regex_last,
                               code == REGEX_REPEAT ? regex_code[i].kind : code,
                               regex_code[i].value, regex_code[i].set);
        }
}

/*
        A program that is a run of characters and nothing else.

        regex_compile lays out SAVE 0, the body, SAVE 1, DONE, so the body is
        every instruction between them and it is a fixed string when all of
        them are REGEX_CHAR. An icase compile lowered each value where it
        emitted it, which is what the search below folds against.
*/
static fn regex_find_literal()
{
        b32 body = regex_length_code - 3;

        regex_literal_length = 0;

        if (regex_broken || body < 1 || body > REGEX_LITERAL_MAX)
                return;

        for (b32 i = 1; i <= body; i++)
                if (regex_code[i].code != REGEX_CHAR)
                        return;

        for (b32 i = 1; i <= body; i++)
                regex_literal[i - 1] = regex_code[i].value;

        regex_literal_length = (positive)body;
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
                memory_fill(regex_loop_at, 0,
                            (positive)regex_length_code * sizeof(positive));

        // Only the pattern that begins with ^ and has no branch in front of
        // it: an alternation puts a split there instead, and one of its
        // branches may not be anchored at all.
        regex_anchored = regex_length_code > 1 && regex_code[1].code == REGEX_BOL;

        memory_fill(regex_visited, 0, (positive)regex_length_code);
        regex_first_known = !regex_first_walk(0);

        if (regex_first_known)
                for (positive c = 0; c < 256; c++)
                        regex_first[256 + c] = !regex_first[c];

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

        regex_find_literal();
        regex_literal_anchors = memory_search_prepare(
                regex_literal, regex_literal_length, regex_icase);
}

static bool regex_compile(string_address pattern, bool extended, bool icase,
                          bool escapes, p8 policy)
{
        regex_code = regex_store + regex_pool_used;
        regex_sets = regex_set_store + regex_pool_sets;
        b32 tables = regex_first_used < REGEX_FIRST_MAX ? regex_first_used++
                                                        : REGEX_FIRST_MAX - 1;

        regex_first = regex_first_store[tables];
        regex_last = regex_last_store[tables];
        regex_literal = regex_literal_store[tables];
        memory_fill(regex_first, 0, 256);
        regex_last_known = false;
        regex_length_code = 0;
        regex_set_count = 0;
        regex_group_count = 0;
        regex_extended = extended;
        regex_icase = icase;
        regex_escapes = escapes;
        regex_policy = policy;
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
                return (regex_policy & REGEX_DOT_NEWLINE) || character != '\n';

        if (inst->kind == REGEX_SET)
                return regex_set_has(inst->set, character);

        return (regex_icase ? byte_to_lower(character) : character) == inst->value;
}

static b32 regex_run(b32 pc, positive sp);

/*
        How deep the runner may recurse before it gives up.

        A group under a star costs a few C frames per repetition, and a line
        of two hundred thousand of them ran the machine's stack out and took
        the process with it. This many frames fit in the smallest stack a
        process here is given; past it the match is refused and said so,
        which is the answer GNU gives for the same expression on the same
        line, rather than a crash.
*/
#define REGEX_DEPTH_MAX 20000

static positive regex_depth;
static bool regex_exhausted;

static b32 regex_run_inner(b32 pc, positive sp)
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
                                character = (p8)byte_to_lower(character);

                        if (character != inst->value)
                                return 0;

                        sp++;
                        pc++;
                        continue;
                }

                case REGEX_ANY:
                        if (sp >= regex_text_length ||
                            (!(regex_policy & REGEX_DOT_NEWLINE) &&
                             regex_text[sp] == '\n'))
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
                        if (sp != 0 &&
                            (!(regex_policy & REGEX_LINE_ANCHORS) ||
                             regex_text[sp - 1] != '\n'))
                                return 0;

                        pc++;
                        continue;

                case REGEX_EOL:
                        if (sp != regex_text_length &&
                            (!(regex_policy & REGEX_LINE_ANCHORS) ||
                             regex_text[sp] != '\n'))
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

                        b32 order = regex_icase
                                        ? memory_compare_ascii_case(regex_text + from,
                                                                    regex_text + sp,
                                                                    span)
                                        : memory_compare(regex_text + from,
                                                         regex_text + sp, span);

                        if (order)
                                return 0;

                        sp += span;
                        pc++;
                        continue;
                }

                default:
                        return 0;
                }
        }
}

static b32 regex_run(b32 pc, positive sp)
{
        b32 answer;

        if (regex_depth >= REGEX_DEPTH_MAX)
        {
                regex_exhausted = true;
                return 0;
        }

        regex_depth++;
        answer = regex_run_inner(pc, sp);
        regex_depth--;

        return answer;
}

// A match the runner gave up on is no match, and is said once: the answer
// GNU gives for the same expression on the same line, not a crash.
static bool regex_gave_up()
{
        if (!regex_exhausted)
                return false;

        regex_exhausted = false;
        text_error(null, "regular expression too complex");
        text_status = 2;
        return true;
}

static fn regex_clear_state()
{
        memory_fill(regex_slots, (b8)-1,
                    regex_slot_used * sizeof(regex_slots[0]));

        for (b32 i = 0; i < regex_loop_count; i++)
                regex_loop_at[regex_loop_list[i]] = 0;
}

// Where a fixed string sits, exactly or under ASCII case equivalence. Both
// paths are bounded library searches; grep and sed keep only the range check
// and the offset that belongs to their surrounding regular-expression state.
static string_address text_literal_find(string_address text, positive length,
                                        positive from, string_address want,
                                        positive size, bool icase,
                                        positive2 anchors)
{
        if (from > length || length - from < size)
                return null;

        return icase ? memory_search_ascii_case_prepared(
                               text + from, length - from, want, size,
                               anchors.x)
                     : memory_search_prepared(text + from, length - from,
                                              want, size, anchors.x,
                                              anchors.y);
}

// Leftmost: the first position where the whole pattern succeeds.
static bool regex_search(string_address text, positive length, positive from)
{
        regex_text = text;
        regex_text_length = length;

        if (regex_literal_length)
        {
                string_address found = text_literal_find(text, length, from, regex_literal,
                                                         regex_literal_length, regex_icase,
                                                         regex_literal_anchors);

                if (!found)
                        return false;

                regex_slots[0] = (positive)(found - text);
                regex_slots[1] = regex_slots[0] + regex_literal_length;
                return true;
        }

        for (positive at = from; at <= length; at++)
        {
                if (regex_first_known)
                {
                        at += string_span_max(text + at, length - at,
                                              (const b8 address_to)(regex_first + 256));

                        // The table exists only when a match must eat a
                        // character, so there is nothing left to try.
                        if (at == length)
                                return false;
                }

                regex_clear_state();

                if (regex_run(0, at))
                        return true;

                if (regex_gave_up() || regex_anchored)
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
static bool regex_keep_longest(positive length)
{
        if (!regex_alternates)
                return true;

        positive at = regex_slots[0];
        positive to = regex_slots[1];
        positive kept[REGEX_SLOT_MAX];

        memory_copy_apart(kept, regex_slots, sizeof(kept));

        for (positive stop = length; stop > to; stop--)
        {
                if (regex_last_known && !regex_last[regex_text[stop - 1]])
                        continue;

                regex_clear_state();
                regex_stop_wanted = stop;

                bool got = regex_run(0, at);

                regex_stop_wanted = TEXT_UNSET;

                if (got)
                        return true;
        }

        memory_copy_apart(regex_slots, kept, sizeof(kept));

        return true;
}

static bool regex_search_longest(string_address text, positive length, positive from)
{
        return regex_search(text, length, from) && regex_keep_longest(length);
}

/* Match at this exact byte rather than searching at or after it. tac walks
   possible separator starts from the right, and a bounded prefix matters:
   after choosing the last byte of a run, the same regexp can have a shorter
   valid match immediately before it. */
static bool regex_match_longest(string_address text, positive length,
                                positive at)
{
        regex_text = text;
        regex_text_length = length;
        regex_clear_state();

        return regex_run(0, at) && !regex_gave_up() && regex_keep_longest(length);
}

// What the kernel says about an open descriptor, through the one statx
// layout file.c reads everywhere rather than a struct stat whose field
// offsets differ between x86_64, arm64 and riscv64.
static bool text_handle_facts(positive handle, file_facts address_to facts)
{
        return file_look((bipolar)handle, (string_address) "", AT_EMPTY_PATH,
                         facts);
}

static bool text_directory(positive handle)
{
        file_facts facts;

        return text_handle_facts(handle, address_of facts) &&
               (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
}

static bool text_regular_size(positive handle, positive address_to size)
{
        file_facts facts;
        p8 edge;

        if (!text_handle_facts(handle, address_of facts))
                return false;

        if ((facts.mode & MODE_FORMAT) != MODE_FILE)
                return false;

        positive bytes = facts.size;
        positive blocks = facts.blocks;

        /* procfs and sysfs report dynamic regular files as zero bytes or one
           page. For an unallocated file, prove st_size by finding its last
           byte and EOF; sparse files pass, pseudo files fall back to reading.
           Allocated ordinary files keep wc -c's one-fstat fast path. */
        if (!blocks)
        {
                bipolar before = bytes
                                       ? system_call_4(syscall(pread64), handle,
                                                       (positive)address_of edge,
                                                       1, bytes - 1)
                                       : 1;
                bipolar after = system_call_4(syscall(pread64), handle,
                                              (positive)address_of edge, 1,
                                              bytes);

                if ((bytes && before != 1) || after != 0)
                        return false;
        }

        address_to size = bytes;
        return true;
}

static b32 address_to text_files;
static positive text_files_room;

static fn text_file_add(b32 which)
{
        if (text_files_failed)
                return;

        if (text_files_count >= text_files_room)
        {
                if (text_files_room > positive_max / 2)
                {
                        text_files_failed = true;
                        return;
                }

                positive room = text_files_room ? text_files_room * 2 : 64;

                if (room > positive_max / sizeof(b32))
                {
                        text_files_failed = true;
                        return;
                }

                positive mapped = (positive)memory(room * sizeof(b32));

                if (!mapped || mapped >= (positive)-4095)
                {
                        text_files_failed = true;
                        return;
                }

                b32 address_to grown = (b32 address_to)mapped;

                if (text_files_count)
                        memory_copy_apart(grown, text_files,
                                         text_files_count * sizeof(b32));

                if (text_files)
                        system_call_2(syscall(munmap), (positive)text_files,
                                      text_files_room * sizeof(b32));

                text_files = grown;
                text_files_room = room;
        }

        text_files[text_files_count++] = which;
}

static bool text_files_ready()
{
        if (!text_files_failed)
                return true;

        text_error(null, "too many operands");
        return false;
}

static string_address text_file_name(positive which)
{
        return text_files_count ? program_argument(text_files[which]) : null;
}

// How many inputs a tool walks: the operands it was handed, or the one
// standard input it reads when it was handed none. text_file_name answers
// null for that one, which is what text_open reads as standard input, so the
// no-operand case needs no second loop anywhere.
static b32 text_input_count()
{
        return text_files_count ? (b32)text_files_count : 1;
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
        The power-of-two encodings.

        base64, base32 and six of basenc's seven alphabets are one operation:
        take a fixed number of input bits at a time and use them as an index
        into an alphabet.  Keeping that operation here means the commands do
        not grow three readers, three writers or three almost-equal codecs.

        Encoding walks whole quanta and reserves the exact output span in the
        shared writer.  Even base2's eightfold expansion therefore makes one
        write reservation per four KiB of input, never one output call per
        bit.  Decoding uses text_line as its staging area; that MiB is already
        present for the line tools, so decoding adds neither an allocation nor
        another large buffer to the multicall image.

        Z85 is deliberately outside this engine.  It is radix 85 over a
        thirty-two-bit integer rather than a bit-sliced alphabet, and folding
        a second arithmetic codec into this path would make the common base64
        loop larger for no shared work.
*/
enum
{
        ENCODING_NONE,
        ENCODING_BASE64,
        ENCODING_BASE64URL,
        ENCODING_BASE32,
        ENCODING_BASE32HEX,
        ENCODING_BASE16,
        ENCODING_BASE2MSBF,
        ENCODING_BASE2LSBF,
        ENCODING_Z85,
};

typedef struct
{
        string_address alphabet;
        p8 bits;
        p8 input;
        p8 output;
        bool padding;
        bool low_bit_first;
} encoding_codec;

static const encoding_codec encoding_codecs[] = {
    {null, 0, 0, 0, false, false},
    {(string_address)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
     6, 3, 4, true, false},
    {(string_address)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
     6, 3, 4, true, false},
    {(string_address)"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567",
     5, 5, 8, true, false},
    {(string_address)"0123456789ABCDEFGHIJKLMNOPQRSTUV",
     5, 5, 8, true, false},
    {(string_address)"0123456789ABCDEF", 4, 1, 2, false, false},
    {(string_address)"01", 1, 1, 8, false, false},
    {(string_address)"01", 1, 1, 8, false, true},
};

/* Two adjacent encoded symbols as one little-endian store.  Every ABI this
   project supports is little-endian, and the no-wrap kernels always advance
   by complete four/eight-byte output quanta, so these halfword stores stay
   aligned.  One table is enough for both widths and all alphabets. */
static p16 encoding_pairs[4096];
static string_address encoding_pairs_alphabet;
static p8 encoding_pairs_bits;

static fn encoding_pairs_prepare(const encoding_codec address_to codec)
{
        if (encoding_pairs_alphabet == codec->alphabet &&
            encoding_pairs_bits == codec->bits)
                return;

        positive symbols = (positive)1 << codec->bits;
        positive entries = symbols * symbols;
        positive mask = symbols - 1;

        for (positive value = 0; value < entries; value++)
                encoding_pairs[value] =
                    (p16)(codec->alphabet[value >> codec->bits] |
                          ((p16)codec->alphabet[value & mask] << 8));

        encoding_pairs_alphabet = codec->alphabet;
        encoding_pairs_bits = codec->bits;
}

typedef struct
{
        positive wrap;
        positive column;
        bool wrote;
} encoding_output;

static positive encoding_wrapped(encoding_output address_to output,
                                  positive symbols)
{
        if (!output->wrap)
                return symbols;

        return symbols + (output->column + symbols) / output->wrap;
}

static inline INLINE p8 address_to encoding_symbol(
    p8 address_to into, encoding_output address_to output, p8 value)
{
        *into++ = value;
        output->wrote = true;

        if (output->wrap && ++output->column == output->wrap)
        {
                *into++ = '\n';
                output->column = 0;
        }

        return into;
}

/* Whole quanta have no carried bits.  That lets every call reserve its exact
   result and lets one block-level dispatch select a fixed-shift kernel. */
static bool encoding_groups(const encoding_codec address_to codec,
                            encoding_output address_to output,
                            p8 address_to input, positive groups)
{
        positive symbols = groups * codec->output;
        p8 address_to into = text_reserve(encoding_wrapped(output, symbols));
        string_address alphabet = codec->alphabet;

        if (!into)
                return false;

        /* -w0 is common in machine-to-machine paths.  Keep its inner loops
           completely free of column updates and wrap branches. */
        if (!output->wrap)
        {
                if (codec->bits == 6)
                        for (positive group = 0; group < groups; group++)
                        {
                                positive bits = ((positive)input[0] << 16) |
                                                ((positive)input[1] << 8) |
                                                input[2];

                                address_to (p16 address_to)into =
                                    encoding_pairs[bits >> 12];
                                address_to (p16 address_to)(into + 2) =
                                    encoding_pairs[bits & 4095];
                                input += 3;
                                into += 4;
                        }
                else if (codec->bits == 5)
                        for (positive group = 0; group < groups; group++)
                        {
                                positive bits = ((positive)input[0] << 32) |
                                                ((positive)input[1] << 24) |
                                                ((positive)input[2] << 16) |
                                                ((positive)input[3] << 8) |
                                                input[4];

                                address_to (p16 address_to)into =
                                    encoding_pairs[bits >> 30];
                                address_to (p16 address_to)(into + 2) =
                                    encoding_pairs[(bits >> 20) & 1023];
                                address_to (p16 address_to)(into + 4) =
                                    encoding_pairs[(bits >> 10) & 1023];
                                address_to (p16 address_to)(into + 6) =
                                    encoding_pairs[bits & 1023];
                                input += 5;
                                into += 8;
                        }
                else if (codec->bits == 4)
                        for (positive group = 0; group < groups; group++)
                        {
                                p8 value = *input++;

                                *into++ = alphabet[value >> 4];
                                *into++ = alphabet[value & 15];
                        }
                else
                        for (positive group = 0; group < groups; group++)
                        {
                                p8 value = *input++;

                                if (codec->low_bit_first)
                                        for (positive bit = 0; bit < 8; bit++)
                                                *into++ = alphabet[
                                                    (value >> bit) & 1];
                                else
                                        for (positive bit = 8; bit; bit--)
                                                *into++ = alphabet[
                                                    (value >> (bit - 1)) & 1];
                        }

                output->wrote = true;
                return true;
        }

        positive wrap = output->wrap;
        positive column = output->column;

#define ENCODING_GROUP_SYMBOL(value)                                        \
        do                                                                  \
        {                                                                   \
                *into++ = (value);                                          \
                if (++column == wrap)                                       \
                {                                                           \
                        *into++ = '\n';                                      \
                        column = 0;                                         \
                }                                                           \
        } while (0)

        /* Dispatch once per block, not once per symbol.  These fixed shifts
           are the steady-state kernels; base64url and base32hex differ only
           in the alphabet pointer and share the same code. */
        if (codec->bits == 6)
        {
                for (positive group = 0; group < groups; group++)
                {
                        positive bits = ((positive)input[0] << 16) |
                                        ((positive)input[1] << 8) | input[2];

                        ENCODING_GROUP_SYMBOL(alphabet[bits >> 18]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 12) & 63]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 6) & 63]);
                        ENCODING_GROUP_SYMBOL(alphabet[bits & 63]);
                        input += 3;
                }
        }
        else if (codec->bits == 5)
        {
                for (positive group = 0; group < groups; group++)
                {
                        positive bits = ((positive)input[0] << 32) |
                                        ((positive)input[1] << 24) |
                                        ((positive)input[2] << 16) |
                                        ((positive)input[3] << 8) | input[4];

                        ENCODING_GROUP_SYMBOL(alphabet[bits >> 35]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 30) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 25) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 20) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 15) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 10) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[(bits >> 5) & 31]);
                        ENCODING_GROUP_SYMBOL(alphabet[bits & 31]);
                        input += 5;
                }
        }
        else if (codec->bits == 4)
        {
                for (positive group = 0; group < groups; group++)
                {
                        p8 value = *input++;

                        ENCODING_GROUP_SYMBOL(alphabet[value >> 4]);
                        ENCODING_GROUP_SYMBOL(alphabet[value & 15]);
                }
        }
        else
        {
                for (positive group = 0; group < groups; group++)
                {
                        p8 value = *input++;

                        if (codec->low_bit_first)
                                for (positive bit = 0; bit < 8; bit++)
                                        ENCODING_GROUP_SYMBOL(
                                            alphabet[(value >> bit) & 1]);
                        else
                                for (positive bit = 8; bit; bit--)
                                        ENCODING_GROUP_SYMBOL(alphabet[
                                            (value >> (bit - 1)) & 1]);
                }
        }

        output->column = column;
        output->wrote = true;
#undef ENCODING_GROUP_SYMBOL
        return true;
}

static bool encoding_tail(const encoding_codec address_to codec,
                          encoding_output address_to output,
                          p8 address_to input, positive count)
{
        if (!count)
                return true;

        positive symbols = (count * 8 + codec->bits - 1) / codec->bits;
        positive padding = codec->padding ? codec->output - symbols : 0;
        positive all = symbols + padding;
        p8 address_to into = text_reserve(encoding_wrapped(output, all));
        positive bits = 0;

        if (!into)
                return false;

        for (positive byte = 0; byte < count; byte++)
                bits = (bits << 8) | input[byte];

        bits <<= symbols * codec->bits - count * 8;

        for (positive symbol = 0; symbol < symbols; symbol++)
        {
                positive shift = (symbols - symbol - 1) * codec->bits;
                positive mask = ((positive)1 << codec->bits) - 1;

                into = encoding_symbol(
                    into, output,
                    codec->alphabet[(bits >> shift) & mask]);
        }

        while (padding--)
                into = encoding_symbol(into, output, '=');

        return true;
}

static b32 encoding_encode(const encoding_codec address_to codec,
                           positive wrap)
{
        /* Five is the widest input quantum (base32). */
        p8 pending[5];
        positive held = 0;
        encoding_output output = {.wrap = wrap};
        bool writing = true;

        if (!wrap && (codec->bits == 6 || codec->bits == 5))
                encoding_pairs_prepare(codec);

        while (writing && text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;

                if (held)
                {
                        positive need = codec->input - held;
                        positive take = left < need ? left : need;

                        memory_copy_apart(pending + held, at, take);
                        held += take;
                        at += take;
                        left -= take;

                        if (held == codec->input)
                        {
                                writing = encoding_groups(codec, address_of output,
                                                          pending, 1);
                                held = 0;
                        }
                }

                while (writing && left >= codec->input)
                {
                        positive groups = left / codec->input;

                        /* At wrap width one, 4096 base2 groups reserve exactly
                           TEXT_OUT_MAX bytes including their newlines. */
                        if (groups > 4096)
                                groups = 4096;

                        writing = encoding_groups(codec, address_of output, at,
                                                  groups);
                        positive used = groups * codec->input;

                        at += used;
                        left -= used;
                }

                if (writing && left)
                {
                        memory_copy_apart(pending, at, left);
                        held = left;
                        at += left;
                        left = 0;
                }

                text_input.position = (positive)(at - text_input.buffer);
        }

        if (writing)
                writing = encoding_tail(codec, address_of output, pending, held);

        /* A positive wrap always terminates the final nonempty short line.
           Width zero means exactly the encoded bytes, with no final newline. */
        if (writing && output.wrap && output.wrote && output.column)
                text_put_character('\n');

        return text_done((!writing || text_status) ? 1 : 0);
}

static bool encoding_padding(const encoding_codec address_to codec,
                             positive residue, positive padding)
{
        positive expected;

        if (!codec->padding)
                return !padding && residue == 0;

        if (!residue)
                expected = 0;
        else if (codec->bits == 6 && residue == 2)
                expected = 2;
        else if (codec->bits == 6 && residue == 3)
                expected = 1;
        else if (codec->bits == 5 && residue == 2)
                expected = 6;
        else if (codec->bits == 5 && residue == 4)
                expected = 4;
        else if (codec->bits == 5 && residue == 5)
                expected = 3;
        else if (codec->bits == 5 && residue == 7)
                expected = 1;
        else
                return false;

        return padding == expected;
}

/* Decode contiguous complete quanta straight into the shared staging area.
   A wrapped line, padding or garbage stops before consuming its group and the
   scalar state machine below handles that edge.  The OR folds each group's
   validation to one branch: every alphabet value is below 64, while an
   unrecognized byte maps to 255. */
static positive encoding_decode_groups(
    const encoding_codec address_to codec, const p8 address_to values,
    p8 address_to input, positive groups, p8 address_to output)
{
        positive done = 0;

        if (codec->bits == 6)
        {
                for (; done < groups; done++)
                {
                        positive a = values[input[0]];
                        positive b = values[input[1]];
                        positive c = values[input[2]];
                        positive d = values[input[3]];

                        if ((a | b | c | d) == 255)
                                break;

                        positive bits = (a << 18) | (b << 12) | (c << 6) | d;

                        output[0] = (p8)(bits >> 16);
                        output[1] = (p8)(bits >> 8);
                        output[2] = (p8)bits;
                        input += 4;
                        output += 3;
                }
        }
        else if (codec->bits == 5)
        {
                for (; done < groups; done++)
                {
                        positive a = values[input[0]];
                        positive b = values[input[1]];
                        positive c = values[input[2]];
                        positive d = values[input[3]];
                        positive e = values[input[4]];
                        positive f = values[input[5]];
                        positive g = values[input[6]];
                        positive h = values[input[7]];

                        if ((a | b | c | d | e | f | g | h) == 255)
                                break;

                        positive bits = (a << 35) | (b << 30) | (c << 25) |
                                        (d << 20) | (e << 15) | (f << 10) |
                                        (g << 5) | h;

                        output[0] = (p8)(bits >> 32);
                        output[1] = (p8)(bits >> 24);
                        output[2] = (p8)(bits >> 16);
                        output[3] = (p8)(bits >> 8);
                        output[4] = (p8)bits;
                        input += 8;
                        output += 5;
                }
        }
        else if (codec->bits == 4)
        {
                for (; done < groups; done++)
                {
                        positive high = values[input[0]];
                        positive low = values[input[1]];

                        if ((high | low) == 255)
                                break;

                        *output++ = (p8)((high << 4) | low);
                        input += 2;
                }
        }
        else
        {
                for (; done < groups; done++)
                {
                        positive made = 0;
                        positive valid = 0;

                        if (codec->low_bit_first)
                                for (positive bit = 0; bit < 8; bit++)
                                {
                                        positive value = values[input[bit]];

                                        valid |= value;
                                        made |= value << bit;
                                }
                        else
                                for (positive bit = 0; bit < 8; bit++)
                                {
                                        positive value = values[input[bit]];

                                        valid |= value;
                                        made = (made << 1) | value;
                                }

                        if (valid == 255)
                                break;

                        *output++ = (p8)made;
                        input += 8;
                }
        }

        return done;
}

static b32 encoding_decode(const encoding_codec address_to codec,
                           bool ignore_garbage)
{
        p8 values[256];
        positive symbols = (positive)1 << codec->bits;
        positive accumulator = 0;
        positive held = 0;
        positive seen = 0;
        positive padding = 0;
        positive made = 0;
        bool padded = false;
        bool valid = true;

        memory_fill(values, 255, sizeof(values));

        for (positive value = 0; value < symbols; value++)
                values[codec->alphabet[value]] = (p8)value;

        while (valid && text_fill())
        {
                while (text_input.position < text_input.filled)
                {
                        if (!padded && !held)
                        {
                                positive left = text_input.filled -
                                                text_input.position;
                                positive groups = left / codec->output;
                                positive room = (TEXT_READ_MAX - made) /
                                                codec->input;

                                if (groups > room)
                                        groups = room;

                                if (groups)
                                {
                                        positive decoded = encoding_decode_groups(
                                            codec, values,
                                            text_input.buffer + text_input.position,
                                            groups, text_line + made);

                                        if (decoded)
                                        {
                                                text_input.position +=
                                                    decoded * codec->output;
                                                made += decoded * codec->input;
                                                seen += decoded * codec->output;

                                                if (made == TEXT_READ_MAX)
                                                {
                                                        text_put(text_line, made);
                                                        made = 0;
                                                }

                                                continue;
                                        }
                                }
                        }

                        p8 byte = text_input.buffer[text_input.position++];
                        p8 value = values[byte];

                        /* GNU's decoders always accept wrapped input.  Other
                           unrecognized bytes need -i. */
                        if (byte == '\n')
                                continue;

                        if (value != 255)
                        {
                                if (padded)
                                {
                                        valid = false;
                                        break;
                                }

                                accumulator = (accumulator << codec->bits) | value;
                                held += codec->bits;
                                seen++;

                                if (held >= 8)
                                {
                                        held -= 8;
                                        text_line[made++] = (p8)(accumulator >> held);

                                        if (made == TEXT_READ_MAX)
                                        {
                                                text_put(text_line, made);
                                                made = 0;
                                        }

                                        if (held)
                                                accumulator &= ((positive)1 << held) - 1;
                                        else
                                                accumulator = 0;
                                }

                                continue;
                        }

                        if (byte == '=' && codec->padding)
                        {
                                padded = true;
                                padding++;
                                continue;
                        }

                        if (!ignore_garbage)
                        {
                                valid = false;
                                break;
                        }
                }
        }

        if (made)
                text_put(text_line, made);

        if (!encoding_padding(codec, seen % codec->output, padding))
                valid = false;

        if (!valid)
                text_error(null, "invalid input");

        return text_done((!valid || text_status) ? 1 : 0);
}

static const file_long encoding_plain_longs[] = {
    {(string_address)"decode", 'd'},
    {(string_address)"ignore-garbage", 'i'},
    {(string_address)"wrap", 'w'},
    {null, 0},
};

static const file_long basenc_longs[] = {
    {(string_address)"decode", 'd'},
    {(string_address)"ignore-garbage", 'i'},
    {(string_address)"wrap", 'w'},
    {(string_address)"base64", '6'},
    {(string_address)"base64url", 'u'},
    {(string_address)"base32", '3'},
    {(string_address)"base32hex", 'x'},
    {(string_address)"base16", 'h'},
    {(string_address)"base2msbf", 'm'},
    {(string_address)"base2lsbf", 'l'},
    {(string_address)"z85", 'z'},
    {null, 0},
};

static b32 text_encoding(string_address name, positive format)
{
        file_taking taking = {
            .program = name,
            .allowed = (string_address)"diw",
            .valued = (string_address)"w",
            .longs = format == ENCODING_NONE ? basenc_longs
                                             : encoding_plain_longs,
        };
        positive wrap = 76;

        text_begin(name);

        if (!file_take(address_of taking))
                return text_done(1);

        if (format == ENCODING_NONE)
        {
                const p8 choices[] = {'6', 'u', '3', 'x', 'h', 'm', 'l', 'z'};

                for (positive at = 0; at < sizeof(choices); at++)
                        if (taking.flags & FILE_FLAG(choices[at]))
                        {
                                if (format != ENCODING_NONE)
                                        return text_refuse(null,
                                                           "multiple encoding types",
                                                           1);

                                format = at + ENCODING_BASE64;
                        }

                if (format == ENCODING_NONE)
                        return text_refuse(null, "missing encoding type", 1);
        }

        if (format == ENCODING_Z85)
                return text_refuse(null, "z85 encoding is not supported", 1);

        string_address said = file_option_value(address_of taking, 'w');

        if (said && !text_unsigned_option(said, false, address_of wrap))
                return text_refuse(said, "invalid wrap size", 1);

        positive operands = (positive)text_argument_count - taking.first;

        if (operands > 1)
                return text_refuse(program_argument((b32)taking.first + 1),
                                   "extra operand", 1);

        string_address path = operands
            ? program_argument((b32)taking.first)
            : null;

        if (!text_open(path))
                return text_done(1);

        b32 answered = taking.flags & FILE_FLAG('d')
            ? encoding_decode(address_of encoding_codecs[format],
                              (taking.flags & FILE_FLAG('i')) != 0)
            : encoding_encode(address_of encoding_codecs[format], wrap);

        text_close();
        return answered;
}

static b32 text_base64()
{
        return text_encoding((string_address)"base64", ENCODING_BASE64);
}

static b32 text_base32()
{
        return text_encoding((string_address)"base32", ENCODING_BASE32);
}

static b32 text_basenc()
{
        return text_encoding((string_address)"basenc", ENCODING_NONE);
}

/*
        Records from more than one input.

        The ordinary line tools above have one global reader.  comm, paste
        and join need several live positions, but not another way of doing
        I/O: each cursor is the same text_reader and the same 64 KiB refill.
        A record wholly inside a refill is returned as a view.  Only one that
        crosses a refill is copied into the caller's existing line-sized
        spill buffer, so the ordered steady-state walk does no allocation and
        no record copy.

        `previous` is optional.  A merge walker gives it the record it is
        about to replace; that record is preserved only when this advance is
        going to refill its reader (or reuse its spill).  This is the same
        boundary rule text_line_view uses for uniq, generalized to a named
        reader rather than duplicated in three applets.
*/
typedef struct
{
        text_reader reader;
        p8 address_to spill;
        /* paste aliases repeated '-' operands to one cursor so its 64 KiB
           reads are shared instead of making byte-sized reads or letting
           independent buffers steal chunks from the same descriptor. */
        positive source;
        p8 address_to record;
        positive length;
        /* join parses the key once when a record becomes current.  Other
           relation applets leave these alone. */
        p8 address_to key;
        positive key_length;
        bool ended;
        bool have;
} text_record_cursor;

static bool text_record_open(text_record_cursor address_to cursor,
                             string_address path, p8 address_to spill)
{
        cursor->spill = spill;
        cursor->source = 0;
        cursor->record = null;
        cursor->length = 0;
        cursor->key = null;
        cursor->key_length = 0;
        cursor->ended = false;
        cursor->have = false;
        return text_reader_open(address_of cursor->reader, path);
}

static fn text_record_close(text_record_cursor address_to cursor)
{
        text_close_handle(address_of cursor->reader.opened,
                          cursor->reader.handle);
}

static fn text_record_preserve(p8 address_to address_to record,
                               positive length, p8 address_to storage)
{
        if (!record || !address_to record || address_to record == storage)
                return;

        memory_copy(storage, address_to record, length);
        address_to record = storage;
}

static bool text_record_next(text_record_cursor address_to cursor,
                             p8 delimiter,
                             p8 address_to address_to previous,
                             positive previous_length,
                             p8 address_to previous_storage)
{
        text_reader address_to reader = address_of cursor->reader;
        positive used = 0;

        cursor->record = null;
        cursor->length = 0;
        cursor->ended = false;
        cursor->have = false;

        /* A complete next record can be taken without changing this fill.
           Otherwise preserve a view the caller still needs before fill
           overwrites it.  A split prior record already occupies spill, which
           the next split record will reuse. */
        if (previous && address_to previous)
        {
                positive left = reader->filled - reader->position;
                bool complete = left && memory_first_of(
                    reader->buffer + reader->position, delimiter, left);

                if (address_to previous == cursor->spill || !complete)
                        text_record_preserve(previous, previous_length,
                                             previous_storage);
        }

        if (!text_reader_fill(reader))
                return false;

        for (;;)
        {
                p8 address_to at = reader->buffer + reader->position;
                positive left = reader->filled - reader->position;
                p8 address_to found = memory_first_of(at, delimiter, left);
                positive take = found ? (positive)(found - at) : left;

                if (!used && found)
                {
                        cursor->record = at;
                        cursor->length = take;
                        cursor->ended = true;
                        cursor->have = true;
                        reader->position += take + 1;
                        return true;
                }

                if (take > TEXT_LINE_MAX - used)
                {
                        reader->position = reader->filled;
                        reader->finished = true;
                        reader->failed = true;
                        text_error(reader->name, "line too long");
                        return false;
                }

                memory_copy(cursor->spill + used, at, take);
                used += take;
                reader->position += take;

                if (found)
                {
                        reader->position++;
                        cursor->record = cursor->spill;
                        cursor->length = used;
                        cursor->ended = true;
                        cursor->have = true;
                        return true;
                }

                if (!text_reader_fill(reader))
                {
                        if (!used)
                                return false;

                        cursor->record = cursor->spill;
                        cursor->length = used;
                        cursor->have = true;
                        return true;
                }
        }
}

/* A lookahead that is free when the next record is already in this refill.
   It never fills and never changes the cursor, so a merge can prove its two
   current keys are unique without copying either record. */
static bool text_record_peek(text_record_cursor address_to cursor,
                             p8 delimiter, p8 address_to address_to record,
                             positive address_to length)
{
        text_reader address_to reader = address_of cursor->reader;

        if (reader->position >= reader->filled)
                return false;

        p8 address_to at = reader->buffer + reader->position;
        positive left = reader->filled - reader->position;
        p8 address_to found = memory_first_of(at, delimiter, left);

        if (!found)
                return false;

        address_to record = at;
        address_to length = (positive)(found - at);
        return true;
}

static fn text_record_take_peeked(text_record_cursor address_to cursor,
                                  p8 address_to record, positive length)
{
        cursor->record = record;
        cursor->length = length;
        cursor->ended = true;
        cursor->have = true;
        cursor->reader.position += length + 1;
}

static bipolar text_record_compare(p8 address_to one, positive one_length,
                                    p8 address_to two, positive two_length,
                                    bool fold)
{
        positive shared = min(one_length, two_length);
        bipolar order = fold ? memory_compare_ascii_case(one, two, shared)
                             : memory_compare(one, two, shared);

        if (order)
                return order;

        return one_length < two_length ? -1 : one_length > two_length;
}

/* text_line is idle while a relation applet runs and is the first cursor's
   refill-spanning store.  Only the second spill and one transient prior need
   new storage: the prior is consumed by the comparison before another side
   advances, so it is shared by both sides. */
static p8 relation_spill[TEXT_LINE_MAX + 1];
/* uniq already needed one retained record.  The merge walkers use the same
   mutually-exclusive store for the one prior record crossing a refill. */
static p8 text_record_hold[TEXT_LINE_MAX + 1];

/* comm ---------------------------------------------------- */

static const file_long comm_longs[] = {
    {(string_address)"check-order", 'C'},
    {(string_address)"nocheck-order", 'N'},
    {(string_address)"output-delimiter", 'O'},
    {(string_address)"total", 'T'},
    {(string_address)"zero-terminated", 'z'},
    {null, 0},
};

enum
{
        RELATION_ORDER_DEFAULT,
        RELATION_ORDER_FORCE,
        RELATION_ORDER_NONE,
};

static positive comm_order_mode;

static bool comm_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'C')
                comm_order_mode = RELATION_ORDER_FORCE;
        else if (letter == 'N')
                comm_order_mode = RELATION_ORDER_NONE;

        return true;
}

static fn comm_separator(string_address delimiter)
{
        /* GNU uses one NUL when an explicitly empty delimiter is named. */
        if (!delimiter[0])
                text_put_character('\0');
        else
                text_put(delimiter, string_length(delimiter));
}

static fn comm_record(p8 address_to record, positive length, positive column,
                      positive shown_columns, string_address separator,
                      p8 delimiter)
{
        for (positive at = 0; at < column && at < shown_columns; at++)
                comm_separator(separator);

        text_put(record, length);
        text_put_character(delimiter);
}

static bool comm_advance(text_record_cursor address_to cursor,
                         p8 delimiter, bool check,
                         bool address_to disorder)
{
        p8 address_to old = cursor->record;
        positive old_length = cursor->length;
        bool more = text_record_next(
            cursor, delimiter, check ? address_of old : null,
            old_length, text_record_hold);

        if (more && check && text_record_compare(old, old_length,
                                                  cursor->record,
                                                  cursor->length, false) > 0)
        {
                address_to disorder = true;
        }

        return more;
}

static b32 text_comm()
{
        file_taking taking = {
            .program = (string_address)"comm",
            .allowed = (string_address)"123z",
            .valued = (string_address)"O",
            .longs = comm_longs,
            .operand = text_file_add,
            .seen = comm_option_seen,
        };

        text_begin("comm");
        text_delimiter = '\n';
        comm_order_mode = RELATION_ORDER_DEFAULT;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        if (text_files_count < 2)
                return text_refuse(null, "missing operand", 1);
        if (text_files_count > 2)
                return text_refuse(text_file_name(2), "extra operand", 1);

        string_address left_name = text_file_name(0);
        string_address right_name = text_file_name(1);

        if (string_equals(left_name, "-") && string_equals(right_name, "-"))
                return text_refuse(null, "standard input is meaningful only once", 1);

        if (taking.flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        text_record_cursor sides[2];

        if (!text_record_open(sides, left_name, text_line))
                return text_done(1);
        if (!text_record_open(sides + 1, right_name, relation_spill))
        {
                text_record_close(sides);
                return text_done(1);
        }

        string_address separator = file_option_value(address_of taking, 'O');

        if (!separator)
                separator = (string_address)"\t";

        bool show[3] = {
            !(taking.flags & FILE_FLAG('1')),
            !(taking.flags & FILE_FLAG('2')),
            !(taking.flags & FILE_FLAG('3')),
        };
        positive column[3];
        positive before = 0;

        for (positive at = 0; at < 3; at++)
        {
                column[at] = before;
                if (show[at])
                        before++;
        }

        positive totals[3] = {0, 0, 0};
        bool disorder = false;
        bool unpaired = false;
        bool have_left = text_record_next(sides, text_delimiter,
                                           null, 0, null);
        bool have_right = text_record_next(sides + 1, text_delimiter,
                                            null, 0, null);

        while (have_left && have_right)
        {
                bipolar order = text_record_compare(
                    sides[0].record, sides[0].length,
                    sides[1].record, sides[1].length, false);
                positive which = order < 0 ? 0 : order > 0 ? 1 : 2;

                unpaired |= which != 2;

                totals[which]++;
                if (show[which])
                        comm_record(sides[which == 1].record,
                                    sides[which == 1].length,
                                    column[which], before, separator,
                                    text_delimiter);

                if (which != 1)
                        have_left = comm_advance(
                            sides, text_delimiter,
                            comm_order_mode != RELATION_ORDER_NONE,
                            address_of disorder);
                if (disorder && comm_order_mode == RELATION_ORDER_FORCE)
                        break;
                if (which != 0)
                        have_right = comm_advance(
                            sides + 1, text_delimiter,
                            comm_order_mode != RELATION_ORDER_NONE,
                            address_of disorder);

                if (disorder && comm_order_mode == RELATION_ORDER_FORCE)
                        break;
        }

        while (have_left &&
               !(disorder && comm_order_mode == RELATION_ORDER_FORCE))
        {
                unpaired = true;
                totals[0]++;
                if (show[0])
                        comm_record(sides[0].record, sides[0].length,
                                    column[0], before, separator,
                                    text_delimiter);
                have_left = comm_advance(sides, text_delimiter,
                                         comm_order_mode != RELATION_ORDER_NONE,
                                         address_of disorder);
        }

        while (have_right &&
               !(disorder && comm_order_mode == RELATION_ORDER_FORCE))
        {
                unpaired = true;
                totals[1]++;
                if (show[1])
                        comm_record(sides[1].record, sides[1].length,
                                    column[1], before, separator,
                                    text_delimiter);
                have_right = comm_advance(sides + 1, text_delimiter,
                                          comm_order_mode != RELATION_ORDER_NONE,
                                          address_of disorder);
        }

        if (taking.flags & FILE_FLAG('T'))
        {
                positive_to_string(text_put, totals[0]);
                comm_separator(separator);
                positive_to_string(text_put, totals[1]);
                comm_separator(separator);
                positive_to_string(text_put, totals[2]);
                comm_separator(separator);
                text_put_string("total");
                text_put_character(text_delimiter);
        }

        bool failed = sides[0].reader.failed || sides[1].reader.failed;
        bool order_failed = disorder &&
            (comm_order_mode == RELATION_ORDER_FORCE || unpaired);

        if (order_failed)
                text_error(null, "input is not in sorted order");

        text_record_close(sides);
        text_record_close(sides + 1);
        return text_done((failed || order_failed) ? 1 : 0);
}

/* paste --------------------------------------------------- */

static const file_long paste_longs[] = {
    {(string_address)"delimiters", 'd'},
    {(string_address)"serial", 's'},
    {(string_address)"zero-terminated", 'z'},
    {null, 0},
};

/* 256 is the empty delimiter.  A NUL delimiter is still the byte zero. */
#define PASTE_EMPTY 256

static bool paste_delimiters(string_address said, p16 address_to made,
                             positive room, positive address_to count)
{
        positive at = 0;
        positive have = 0;

        while (said[at])
        {
                positive value = (p8)said[at++];

                if (value == '\\')
                {
                        p8 escaped = said[at++];

                        if (!escaped)
                                return false;

                        p8 simple = byte_simple_escape(escaped);

                        if (escaped == '0' &&
                                 !byte_is_digit(said[at]))
                                value = PASTE_EMPTY;
                        else if (simple)
                                value = simple;
                        else if (escaped >= '0' && escaped <= '7')
                        {
                                value = escaped - '0';

                                for (positive digit = 1; digit < 3 &&
                                     said[at] >= '0' && said[at] <= '7'; digit++)
                                        value = value * 8 + said[at++] - '0';

                                value &= 255;
                        }
                        else
                                value = escaped;
                }

                if (have >= room)
                        return false;

                made[have++] = (p16)value;
        }

        if (!have)
                made[have++] = PASTE_EMPTY;

        address_to count = have;
        return true;
}

static fn paste_delimiter(p16 address_to delimiters, positive count,
                          positive which)
{
        positive value = delimiters[which % count];

        if (value != PASTE_EMPTY)
                text_put_character((p8)value);
}

static b32 text_paste()
{
        file_taking taking = {
            .program = (string_address)"paste",
            .allowed = (string_address)"dsz",
            .valued = (string_address)"d",
            .longs = paste_longs,
            .operand = text_file_add,
        };

        text_begin("paste");
        text_delimiter = '\n';
        text_arena_used = 0;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        if (taking.flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        positive delimiter_count;
        string_address said = file_option_value(address_of taking, 'd');
        positive delimiter_room = said ? string_length(said) : 1;

        if (!delimiter_room)
                delimiter_room = 1;
        if (delimiter_room > positive_max / sizeof(p16))
                return text_refuse(said, "invalid delimiter list", 1);

        p16 address_to delimiters = (p16 address_to)text_arena_take(
            delimiter_room * sizeof(p16));

        if (!delimiters ||
            !paste_delimiters(said ? said : (string_address)"\t",
                              delimiters, delimiter_room,
                              address_of delimiter_count))
                return text_refuse(said, "invalid delimiter list", 1);

        positive inputs = text_files_count ? text_files_count : 1;

        if (inputs > positive_max / sizeof(text_record_cursor))
                return text_refuse(null, "too many operands", 1);

        text_record_cursor address_to cursors =
            (text_record_cursor address_to)text_arena_take(
                inputs * sizeof(text_record_cursor));

        if (!cursors)
                return text_done(1);

        positive first_standard = TEXT_UNSET;

        for (positive input = 0; input < inputs; input++)
        {
                string_address name = text_files_count
                    ? text_file_name(input) : null;
                bool standard = !name || string_equals(name, "-");

                if (standard && first_standard != TEXT_UNSET)
                {
                        cursors[input].source = first_standard;
                        continue;
                }

                text_record_open(cursors + input, name, text_line);
                cursors[input].source = input;

                if (standard)
                        first_standard = input;
        }

        bool serial = (taking.flags & FILE_FLAG('s')) != 0;

        if (serial)
        {
                for (positive input = 0; input < inputs; input++)
                {
                        text_record_cursor address_to cursor = cursors + input;

                        cursor = cursors + cursor->source;
                        positive field = 0;

                        if (cursor->reader.failed)
                                continue;

                        while (text_record_next(cursor, text_delimiter,
                                                null, 0, null))
                        {
                                if (field)
                                        paste_delimiter(delimiters,
                                                        delimiter_count,
                                                        field - 1);
                                text_put(cursor->record, cursor->length);
                                field++;
                        }

                        text_put_character(text_delimiter);
                }
        }
        else
        {
                for (;;)
                {
                        bool any = false;
                        positive next_separator = 0;

                        for (positive input = 0; input < inputs; input++)
                        {
                                text_record_cursor address_to cursor =
                                    cursors + input;

                                cursor = cursors + cursor->source;

                                if (cursor->reader.failed ||
                                    !text_record_next(cursor, text_delimiter,
                                                      null, 0, null))
                                        continue;

                                while (next_separator < input)
                                        paste_delimiter(delimiters,
                                                        delimiter_count,
                                                        next_separator++);

                                text_put(cursor->record, cursor->length);
                                any = true;
                        }

                        if (!any)
                                break;

                        while (next_separator + 1 < inputs)
                                paste_delimiter(delimiters, delimiter_count,
                                                next_separator++);

                        text_put_character(text_delimiter);
                }
        }

        bool failed = false;

        for (positive input = 0; input < inputs; input++)
        {
                text_record_cursor address_to cursor = cursors + input;

                if (cursor->source != input)
                        continue;

                failed |= cursor->reader.failed;
                text_record_close(cursor);
        }

        return text_done(failed ? 1 : 0);
}

/* join ---------------------------------------------------- */

#define JOIN_OUTPUT_MAX 256
#define JOIN_GROUP_FIRST TEXT_READ_MAX

typedef struct
{
        p8 file;
        positive field;
} join_output;

typedef struct
{
        p8 address_to bytes;
        positive length;
        positive position;
        positive field;
        p8 separator;
        bool separated;
        bool done;
} join_fields;

typedef struct
{
        positive bytes;
        positive length;
} join_stored;

static positive join_key[2];
static positive join_unpaired;
static positive join_only;
static join_output join_outputs[JOIN_OUTPUT_MAX];
static positive join_output_count;
static bool join_output_auto;
static positive join_order_mode;

static bool join_order_check(bool unpaired)
{
        return join_order_mode == RELATION_ORDER_FORCE ||
               (join_order_mode == RELATION_ORDER_DEFAULT && unpaired);
}

static bool join_number(string_address value, positive address_to number)
{
        positive made;

        if (!text_unsigned_option(value, false, address_of made) || !made)
                return false;

        address_to number = made - 1;
        return true;
}

static bool join_output_add(string_address word)
{
        positive at = 0;

        if (string_equals(word, "auto"))
        {
                if (join_output_count)
                        return false;
                join_output_auto = true;
                return true;
        }

        if (join_output_auto)
                return false;

        while (word[at])
        {
                while (word[at] == ',' || byte_is_blank(word[at]))
                        at++;

                if (!word[at])
                        break;
                if (join_output_count == JOIN_OUTPUT_MAX)
                        return false;

                join_output output = {0, 0};

                if (word[at] == '0' &&
                    (!word[at + 1] || word[at + 1] == ',' ||
                     byte_is_blank(word[at + 1])))
                        at++;
                else
                {
                        if ((word[at] != '1' && word[at] != '2') ||
                            word[at + 1] != '.')
                                return false;

                        output.file = word[at] - '0';
                        at += 2;
                        positive start = at;
                        positive field = 0;

                        while (byte_is_digit(word[at]))
                        {
                                positive digit = word[at++] - '0';

                                if (field > (positive_max - digit) / 10)
                                        return false;
                                field = field * 10 + digit;
                        }

                        if (at == start || !field)
                                return false;

                        output.field = field - 1;
                }

                if (word[at] && word[at] != ',' && !byte_is_blank(word[at]))
                        return false;

                join_outputs[join_output_count++] = output;
        }

        return join_output_count != 0;
}

static bool join_side(string_address value, positive address_to mask)
{
        if (value && value[0] == '1' && !value[1])
                address_to mask |= 1;
        else if (value && value[0] == '2' && !value[1])
                address_to mask |= 2;
        else
                return false;

        return true;
}

static bool join_option_seen(p8 letter, string_address value)
{
        if (letter == '1' || letter == '2')
        {
                positive field;

                if (!join_number(value, address_of field))
                        return false;
                join_key[letter - '1'] = field;
        }
        else if (letter == 'j')
        {
                positive field;

                if (!join_number(value, address_of field))
                        return false;
                join_key[0] = join_key[1] = field;
        }
        else if (letter == 'a')
        {
                if (!join_side(value, address_of join_unpaired))
                        return false;
        }
        else if (letter == 'v')
        {
                if (!join_side(value, address_of join_only))
                        return false;
        }
        else if (letter == 'o')
        {
                if (!join_output_add(value))
                        return false;
        }
        else if (letter == 'C')
                join_order_mode = RELATION_ORDER_FORCE;
        else if (letter == 'N')
                join_order_mode = RELATION_ORDER_NONE;

        return true;
}

static const file_long join_longs[] = {
    {(string_address)"ignore-case", 'i'},
    {(string_address)"check-order", 'C'},
    {(string_address)"nocheck-order", 'N'},
    {(string_address)"header", 'H'},
    {(string_address)"zero-terminated", 'z'},
    {null, 0},
};

static fn join_fields_begin(join_fields address_to fields,
                            p8 address_to bytes, positive length,
                            bool separated, p8 separator)
{
        fields->bytes = bytes;
        fields->length = length;
        fields->position = 0;
        fields->field = 0;
        fields->separator = separator;
        fields->separated = separated;
        fields->done = false;
}

static bool join_field_next(join_fields address_to fields,
                            p8 address_to address_to value,
                            positive address_to length)
{
        if (fields->done)
                return false;

        positive at = fields->position;
        positive start;

        if (!fields->separated)
        {
                while (at < fields->length && byte_is_blank(fields->bytes[at]))
                        at++;
                if (at == fields->length)
                {
                        fields->done = true;
                        return false;
                }

                start = at;
                while (at < fields->length && !byte_is_blank(fields->bytes[at]))
                        at++;
                fields->position = at;
        }
        else
        {
                start = at;
                while (at < fields->length &&
                       fields->bytes[at] != fields->separator)
                        at++;

                if (at < fields->length)
                        fields->position = at + 1;
                else
                {
                        fields->position = at;
                        fields->done = true;
                }
        }

        address_to value = fields->bytes + start;
        address_to length = at - start;
        fields->field++;
        return true;
}

static bool join_field_at(p8 address_to line, positive length,
                          positive wanted, bool separated, p8 separator,
                          p8 address_to address_to value,
                          positive address_to value_length)
{
        join_fields fields;
        p8 address_to found = line;
        positive found_length = 0;

        join_fields_begin(address_of fields, line, length,
                          separated, separator);

        for (positive at = 0; at <= wanted; at++)
                if (!join_field_next(address_of fields, address_of found,
                                     address_of found_length))
                {
                        address_to value = line;
                        address_to value_length = 0;
                        return false;
                }

        address_to value = found;
        address_to value_length = found_length;

        return true;
}

static positive join_field_count(p8 address_to line, positive length,
                                 bool separated, p8 separator)
{
        join_fields fields;
        p8 address_to value;
        positive value_length;
        positive count = 0;

        join_fields_begin(address_of fields, line, length,
                          separated, separator);
        while (join_field_next(address_of fields, address_of value,
                               address_of value_length))
                count++;

        return count;
}

static fn join_cursor_key(text_record_cursor address_to cursor,
                          positive side, bool separated, p8 separator)
{
        cursor->key = cursor->record;
        cursor->key_length = 0;
        join_field_at(cursor->record, cursor->length, join_key[side],
                      separated, separator, address_of cursor->key,
                      address_of cursor->key_length);
}

static fn join_view_key(p8 address_to record, positive length, positive side,
                        bool separated, p8 separator,
                        p8 address_to address_to key,
                        positive address_to key_length)
{
        address_to key = record;
        address_to key_length = 0;
        join_field_at(record, length, join_key[side], separated, separator,
                      key, key_length);
}

static fn join_put_field(bool address_to first,
                         p8 address_to value, positive length,
                         bool present, string_address empty,
                         p8 output_separator)
{
        if (!address_to first)
                text_put_character(output_separator);
        address_to first = false;

        if (present)
                text_put(value, length);
        else if (empty)
                text_put(empty, string_length(empty));
}

static fn join_put_nonkeys(bool address_to first, p8 address_to line,
                           positive length, positive key, bool separated,
                           p8 separator, p8 output_separator)
{
        join_fields fields;
        p8 address_to value;
        positive value_length;
        positive field = 0;

        join_fields_begin(address_of fields, line, length,
                          separated, separator);
        while (join_field_next(address_of fields, address_of value,
                               address_of value_length))
        {
                if (field++ == key)
                        continue;

                join_put_field(first, value, value_length, true, null,
                               output_separator);
        }
}

static fn join_emit(p8 address_to left, positive left_length,
                    p8 address_to right, positive right_length,
                    bool separated, p8 separator, string_address empty,
                    positive auto_left, positive auto_right,
                    p8 delimiter)
{
        bool first = true;
        p8 output_separator = separated ? separator : ' ';

        if (join_output_count)
        {
                for (positive at = 0; at < join_output_count; at++)
                {
                        join_output field = join_outputs[at];
                        p8 address_to value = null;
                        positive length = 0;
                        bool present;

                        if (!field.file)
                        {
                                present = left && join_field_at(
                                    left, left_length, join_key[0], separated,
                                    separator, address_of value,
                                    address_of length);
                                if (!present && right)
                                        present = join_field_at(
                                            right, right_length, join_key[1],
                                            separated, separator,
                                            address_of value,
                                            address_of length);
                        }
                        else
                        {
                                p8 address_to line = field.file == 1
                                    ? left : right;
                                positive line_length = field.file == 1
                                    ? left_length : right_length;

                                present = line && join_field_at(
                                    line, line_length, field.field, separated,
                                    separator, address_of value,
                                    address_of length);
                        }

                        join_put_field(address_of first, value, length,
                                       present, empty, output_separator);
                }
        }
        else if (join_output_auto)
        {
                p8 address_to value = null;
                positive length = 0;
                bool present = left && join_field_at(
                    left, left_length, join_key[0], separated, separator,
                    address_of value, address_of length);

                if (!present && right)
                        present = join_field_at(
                            right, right_length, join_key[1], separated,
                            separator, address_of value, address_of length);

                join_put_field(address_of first, value, length, present,
                               empty, output_separator);

                for (positive side = 0; side < 2; side++)
                {
                        p8 address_to line = side ? right : left;
                        positive line_length = side ? right_length : left_length;
                        positive fields = side ? auto_right : auto_left;

                        for (positive field = 0; field < fields; field++)
                        {
                                if (field == join_key[side])
                                        continue;

                                present = line && join_field_at(
                                    line, line_length, field, separated,
                                    separator, address_of value,
                                    address_of length);
                                join_put_field(address_of first, value, length,
                                               present, empty,
                                               output_separator);
                        }
                }
        }
        else
        {
                p8 address_to value = null;
                positive length = 0;
                bool present = left && join_field_at(
                    left, left_length, join_key[0], separated, separator,
                    address_of value, address_of length);

                if (!present && right)
                        present = join_field_at(
                            right, right_length, join_key[1], separated,
                            separator, address_of value, address_of length);

                join_put_field(address_of first, value, length, present,
                               empty, output_separator);

                if (left)
                        join_put_nonkeys(address_of first, left, left_length,
                                         join_key[0], separated, separator,
                                         output_separator);
                if (right)
                        join_put_nonkeys(address_of first, right, right_length,
                                         join_key[1], separated, separator,
                                         output_separator);
        }

        text_put_character(delimiter);
}

static bool join_advance(text_record_cursor address_to cursor,
                         positive side, p8 delimiter, bool check,
                         bool separated, p8 separator, bool fold,
                         bool address_to disorder)
{
        p8 address_to old = cursor->record;
        positive old_length = cursor->length;
        positive key_offset = cursor->key
            ? (positive)(cursor->key - cursor->record) : 0;
        positive old_key_length = cursor->key_length;
        bool more = text_record_next(
            cursor, delimiter, check ? address_of old : null,
            old_length, text_record_hold);

        if (more)
        {
                join_cursor_key(cursor, side, separated, separator);

                if (check && text_record_compare(
                        old + key_offset, old_key_length,
                        cursor->key, cursor->key_length, fold) > 0)
                {
                        address_to disorder = true;
                }
        }

        return more;
}

static positive join_store(p8 address_to address_to buffer,
                           positive address_to room, positive mark,
                           positive used, p8 address_to record,
                           positive length)
{
        positive bytes = sizeof(join_stored) + length;
        positive aligned = (bytes + sizeof(positive) - 1) &
                           ~(sizeof(positive) - 1);

        if (bytes < length || aligned < bytes || used > positive_max - aligned)
                return TEXT_UNSET;

        positive wanted = used + aligned;

        if (wanted > address_to room)
        {
                positive larger = memory_growth(address_to room, wanted,
                                                 JOIN_GROUP_FIRST);
                positive available = TEXT_ARENA_BYTES - mark;

                if (larger > available)
                        larger = wanted <= available ? available : 0;
                if (!larger)
                        return TEXT_UNSET;

                /* The group is the newest arena object, so rewinding grows it
                   at the same address and retains its bytes.  Capacity
                   changes logarithmically; records themselves only advance
                   `used` inside that one object. */
                text_arena_used = mark;
                p8 address_to grown =
                    (p8 address_to)text_arena_take(larger);

                if (!grown)
                        return TEXT_UNSET;

                address_to buffer = grown;
                address_to room = larger;
        }

        join_stored address_to stored =
            (join_stored address_to)(address_to buffer + used);

        stored->bytes = aligned;
        stored->length = length;
        memory_copy(stored + 1, record, length);
        return used + aligned;
}

static b32 text_join()
{
        file_taking taking = {
            .program = (string_address)"join",
            .allowed = (string_address)"12aeijotvz",
            .valued = (string_address)"12aejotv",
            .longs = join_longs,
            .operand = text_file_add,
            .seen = join_option_seen,
        };

        text_begin("join");
        text_delimiter = '\n';
        text_arena_used = 0;
        join_key[0] = join_key[1] = 0;
        join_unpaired = 0;
        join_only = 0;
        join_output_count = 0;
        join_output_auto = false;
        join_order_mode = RELATION_ORDER_DEFAULT;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_refuse(null, "invalid option value", 1);

        if (text_files_count < 2)
                return text_refuse(null, "missing operand", 1);
        if (text_files_count > 2)
                return text_refuse(text_file_name(2), "extra operand", 1);

        string_address left_name = text_file_name(0);
        string_address right_name = text_file_name(1);

        if (string_equals(left_name, "-") && string_equals(right_name, "-"))
                return text_refuse(null, "both files cannot be standard input", 1);

        string_address separator_text = file_option_value(address_of taking, 't');
        bool separated = separator_text != null;
        p8 separator = ' ';

        if (separated)
        {
                if (!separator_text[0] || separator_text[1])
                        return text_refuse(separator_text,
                                           "separator must be one byte", 1);
                separator = separator_text[0];
        }

        if (taking.flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        bool fold = (taking.flags & FILE_FLAG('i')) != 0;
        bool header = (taking.flags & FILE_FLAG('H')) != 0;
        string_address empty = file_option_value(address_of taking, 'e');
        text_record_cursor sides[2];

        if (!text_record_open(sides, left_name, text_line))
                return text_done(1);
        if (!text_record_open(sides + 1, right_name, relation_spill))
        {
                text_record_close(sides);
                return text_done(1);
        }

        positive group_mark = text_arena_used;
        p8 address_to group = null;
        positive group_room = 0;

        bool have_left = text_record_next(sides, text_delimiter,
                                           null, 0, null);
        bool have_right = text_record_next(sides + 1, text_delimiter,
                                            null, 0, null);

        if (have_left)
                join_cursor_key(sides, 0, separated, separator);
        if (have_right)
                join_cursor_key(sides + 1, 1, separated, separator);

        positive auto_left = have_left
            ? join_field_count(sides[0].record, sides[0].length,
                               separated, separator) : 0;
        positive auto_right = have_right
            ? join_field_count(sides[1].record, sides[1].length,
                               separated, separator) : 0;
        bool disorder = false;
        bool unpaired = false;
        bool trouble = false;

        if (header && (have_left || have_right))
        {
                join_emit(have_left ? sides[0].record : null,
                          have_left ? sides[0].length : 0,
                          have_right ? sides[1].record : null,
                          have_right ? sides[1].length : 0,
                          separated, separator, empty,
                          auto_left, auto_right, text_delimiter);

                if (have_left)
                        have_left = join_advance(
                            sides, 0, text_delimiter, false, separated,
                            separator, fold, address_of disorder);
                if (have_right)
                        have_right = join_advance(
                            sides + 1, 1, text_delimiter, false, separated,
                            separator, fold, address_of disorder);
        }

        while (have_left && have_right)
        {
                bipolar order = text_record_compare(
                    sides[0].key, sides[0].key_length,
                    sides[1].key, sides[1].key_length, fold);

                if (order < 0)
                {
                        unpaired = true;
                        if ((join_unpaired | join_only) & 1)
                                join_emit(sides[0].record, sides[0].length,
                                          null, 0, separated, separator, empty,
                                          auto_left, auto_right,
                                          text_delimiter);
                        have_left = join_advance(
                            sides, 0, text_delimiter,
                            join_order_check(unpaired),
                            separated, separator, fold, address_of disorder);
                        if (disorder &&
                            join_order_mode == RELATION_ORDER_FORCE)
                                break;
                        continue;
                }

                if (order > 0)
                {
                        unpaired = true;
                        if ((join_unpaired | join_only) & 2)
                                join_emit(null, 0, sides[1].record,
                                          sides[1].length, separated, separator,
                                          empty, auto_left, auto_right,
                                          text_delimiter);
                        have_right = join_advance(
                            sides + 1, 1, text_delimiter,
                            join_order_check(unpaired),
                            separated, separator, fold, address_of disorder);
                        if (disorder &&
                            join_order_mode == RELATION_ORDER_FORCE)
                                break;
                        continue;
                }

                /* Nearly every package index has unique ordered keys.  When
                   both following records are already visible in their 64 KiB
                   fills and neither repeats this key, emit and advance in
                   place: no duplicate-group arena, no copy, no allocation.
                   Refill boundaries and actual duplicates take the general
                   run-buffering path below. */
                p8 address_to next_left;
                p8 address_to next_right;
                p8 address_to next_left_key;
                p8 address_to next_right_key;
                positive next_left_length;
                positive next_right_length;
                positive next_left_key_length;
                positive next_right_key_length;
                bool peek_left = text_record_peek(
                    sides, text_delimiter, address_of next_left,
                    address_of next_left_length);
                bool peek_right = text_record_peek(
                    sides + 1, text_delimiter, address_of next_right,
                    address_of next_right_length);

                if (peek_left)
                        join_view_key(next_left, next_left_length, 0,
                                      separated, separator,
                                      address_of next_left_key,
                                      address_of next_left_key_length);
                if (peek_right)
                        join_view_key(next_right, next_right_length, 1,
                                      separated, separator,
                                      address_of next_right_key,
                                      address_of next_right_key_length);

                if (peek_left && peek_right &&
                    text_record_compare(next_left_key,
                                        next_left_key_length,
                                        sides[0].key,
                                        sides[0].key_length, fold) &&
                    text_record_compare(sides[1].key,
                                        sides[1].key_length,
                                        next_right_key,
                                        next_right_key_length, fold))
                {
                        bool next_disorder = join_order_check(unpaired) &&
                            (text_record_compare(
                                 sides[0].key, sides[0].key_length,
                                 next_left_key, next_left_key_length,
                                 fold) > 0 ||
                             text_record_compare(
                                 sides[1].key, sides[1].key_length,
                                 next_right_key, next_right_key_length,
                                 fold) > 0);

                        disorder |= next_disorder;

                        if (next_disorder &&
                            join_order_mode == RELATION_ORDER_FORCE)
                        {
                                break;
                        }

                        if (!join_only)
                                join_emit(sides[0].record, sides[0].length,
                                          sides[1].record, sides[1].length,
                                          separated, separator, empty,
                                          auto_left, auto_right,
                                          text_delimiter);

                        text_record_take_peeked(sides, next_left,
                                                next_left_length);
                        sides[0].key = next_left_key;
                        sides[0].key_length = next_left_key_length;

                        text_record_take_peeked(sides + 1, next_right,
                                                next_right_length);
                        sides[1].key = next_right_key;
                        sides[1].key_length = next_right_key_length;

                        continue;
                }

                /* Buffer exactly one right-side equal-key run.  The common
                   unique-key case stores one record, and the left-major
                   Cartesian walk below matches GNU's duplicate ordering. */
                positive group_used = 0;
                p8 address_to group_key = null;
                positive group_key_length = 0;

                text_arena_used = group_mark;
                group = null;
                group_room = 0;

                do
                {
                        positive next = join_store(
                            address_of group, address_of group_room,
                            group_mark, group_used,
                            sides[1].record, sides[1].length);

                        if (next == TEXT_UNSET)
                        {
                                text_error(null, "matching group too large");
                                trouble = true;
                                have_right = false;
                                break;
                        }

                        group_used = next;

                        if (!group_key)
                        {
                                join_stored address_to first =
                                    (join_stored address_to)group;

                                join_view_key((p8 address_to)(first + 1),
                                              first->length, 1, separated,
                                              separator, address_of group_key,
                                              address_of group_key_length);
                        }

                        have_right = join_advance(
                            sides + 1, 1, text_delimiter,
                            join_order_check(unpaired),
                            separated, separator, fold, address_of disorder);
                        if (disorder &&
                            join_order_mode == RELATION_ORDER_FORCE)
                                break;
                } while (have_right && !text_record_compare(
                             sides[0].key, sides[0].key_length,
                             sides[1].key, sides[1].key_length, fold));

                if (trouble ||
                    (disorder && join_order_mode == RELATION_ORDER_FORCE))
                        break;

                do
                {
                        if (!join_only)
                                for (positive at = 0; at < group_used;)
                                {
                                        join_stored address_to stored =
                                            (join_stored address_to)(group + at);

                                        join_emit(sides[0].record,
                                                  sides[0].length,
                                                  (p8 address_to)(stored + 1),
                                                  stored->length, separated,
                                                  separator, empty,
                                                  auto_left, auto_right,
                                                  text_delimiter);
                                        at += stored->bytes;
                                }

                        have_left = join_advance(
                            sides, 0, text_delimiter,
                            join_order_check(unpaired),
                            separated, separator, fold, address_of disorder);
                        if (disorder &&
                            join_order_mode == RELATION_ORDER_FORCE)
                                break;
                } while (have_left && group_used && !text_record_compare(
                             sides[0].key, sides[0].key_length,
                             group_key, group_key_length, fold));
        }

        while (!trouble && have_left &&
               !(disorder && join_order_mode == RELATION_ORDER_FORCE))
        {
                unpaired = true;
                if ((join_unpaired | join_only) & 1)
                        join_emit(sides[0].record, sides[0].length,
                                  null, 0, separated, separator, empty,
                                  auto_left, auto_right, text_delimiter);
                have_left = join_advance(
                    sides, 0, text_delimiter,
                    join_order_check(unpaired),
                    separated, separator, fold, address_of disorder);
        }

        while (!trouble && have_right &&
               !(disorder && join_order_mode == RELATION_ORDER_FORCE))
        {
                unpaired = true;
                if ((join_unpaired | join_only) & 2)
                        join_emit(null, 0, sides[1].record,
                                  sides[1].length, separated, separator, empty,
                                  auto_left, auto_right, text_delimiter);
                have_right = join_advance(
                    sides + 1, 1, text_delimiter,
                    join_order_check(unpaired),
                    separated, separator, fold, address_of disorder);
        }

        bool failed = sides[0].reader.failed || sides[1].reader.failed;
        bool order_failed = disorder &&
            (join_order_mode == RELATION_ORDER_FORCE || unpaired);

        if (order_failed)
                text_error(null, "input is not in sorted order");

        text_record_close(sides);
        text_record_close(sides + 1);
        return text_done((failed || trouble || order_failed) ? 1 : 0);
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

static fn cat_number()
{
        // Six wide and right aligned, then a tab, which is what GNU does and
        // what anything reading the output will expect.
        positive width = positive_digits(cat_line_number);

        if (width < 6)
                width = 6;

        p8 address_to field = text_reserve(width + 1);

        if (field)
        {
                positive length = positive_into_padded(
                    field, cat_line_number, 6, ' ');
                field[length] = '\t';
        }

        cat_line_number++;
}

// A byte as -v spells it: control characters as ^X, the high half as M- and
// then the same rule again. Tab and newline are touched separately by -T.
static fn cat_walked()
{
        p8 visible[TEXT_VISIBLE_MAX];

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
                                text_put(visible, text_visible(visible, value));
                                continue;
                        }

                        text_put_character(value);
                }
        }
}

static const file_long cat_longs[] = {
    {(string_address) "show-all", 'A'},
    {(string_address) "number-nonblank", 'b'},
    {(string_address) "show-ends", 'E'},
    {(string_address) "number", 'n'},
    {(string_address) "squeeze-blank", 's'},
    {(string_address) "show-tabs", 'T'},
    {(string_address) "show-nonprinting", 'v'},
    {null, 0},
};

static b32 text_cat()
{
        file_taking taking = {
            .program = (string_address) "cat",
            // -u asks for unbuffered, which this always is.
            .allowed = (string_address) "AETbenstuv",
            .valued = (string_address) "",
            .longs = cat_longs,
        };

        text_begin("cat");

        if (!file_take(address_of taking))
                return text_done(1);

        b32 first = (b32)taking.first;
        b32 inputs = 0;
        positive flags = taking.flags;

        cat_flags = 0;
        cat_line_number = 1;
        cat_blank_before = false;
        cat_at_line_start = true;

        if (flags & FILE_FLAG('b'))
                cat_flags |= CAT_NUMBER_FULL;
        else if (flags & FILE_FLAG('n'))
                // -b wins over -n, as it does everywhere else.
                cat_flags |= CAT_NUMBER;

        if (flags & (FILE_FLAG('E') | FILE_FLAG('e') | FILE_FLAG('A')))
                cat_flags |= CAT_ENDS;

        if (flags & (FILE_FLAG('T') | FILE_FLAG('t') | FILE_FLAG('A')))
                cat_flags |= CAT_TABS;

        if (flags & (FILE_FLAG('v') | FILE_FLAG('e') | FILE_FLAG('t') |
                     FILE_FLAG('A')))
                cat_flags |= CAT_SHOW;

        if (flags & FILE_FLAG('s'))
                cat_flags |= CAT_SQUEEZE;

        for (b32 i = first; i < text_argument_count; i++)
                inputs++;

        if (!inputs)
        {
                if (text_open(null))
                        cat_flags ? cat_walked() : text_put_rest();

                text_close();
                return text_done(text_status);
        }

        for (b32 i = first; i < text_argument_count; i++)
        {
                if (!text_open(program_argument(i)))
                        continue;

                cat_flags ? cat_walked() : text_put_rest();
                text_close();
        }

        return text_done(text_status);
}

static const file_long wc_longs[] = {
    {(string_address) "lines", 'l'},
    {(string_address) "words", 'w'},
    {(string_address) "bytes", 'c'},
    {(string_address) "chars", 'm'},
    {(string_address) "max-line-length", 'L'},
    {(string_address) "total", 'T'},
    // --debug names the counting strategy on the error stream and leaves
    // the counts alone, and there is one strategy here to name. It borrows a
    // D that `allowed` refuses, so wc -D stays the error GNU makes of it.
    {(string_address) "debug", 'D'},
    {null, 0},
};

/*
        One row of wc's counts.

        wc prints the same row twice under two names -- once for each input,
        once for the total across them -- and the rule is the same both
        times: the columns that were asked for, in the fixed order GNU writes
        them, one space between, each padded to a width settled before
        anything was printed. Written out twice, that rule is two places for
        the total row to come out spaced differently from the rows above it.

        The name is the file the counts came from, "total" for the last row,
        or nothing at all when the counts came from standard input.

        -m and -c are two columns holding the same number: this file counts
        bytes for both, so a character is a byte here and the two flags
        differ only in whether their column appears.
*/
static bool wc_want_lines;
static bool wc_want_words;
static bool wc_want_bytes;
static bool wc_want_chars;
static bool wc_want_longest;

enum
{
        WC_TOTAL_AUTO,
        WC_TOTAL_ALWAYS,
        WC_TOTAL_ONLY,
        WC_TOTAL_NEVER,
};

// GNU accepts an unambiguous prefix of the four --total values. A bare "a"
// is ambiguous between auto and always, while "o" and "n" are already
// enough to choose only and never.
static bool wc_total_of(string_address said, positive address_to mode)
{
        static const string_address names[4] = {
            (string_address) "auto", (string_address) "always",
            (string_address) "only", (string_address) "never"};
        positive length = string_length(said);
        positive matches = 0;

        for (positive i = 0; i < 4; i++)
        {
                if (length <= string_length(names[i]) &&
                    !string_compare_max(said, names[i], length))
                {
                        matches++;
                        address_to mode = i;
                }
        }

        return matches == 1;
}

static fn wc_row(positive lines, positive words, positive bytes,
                 positive longest, positive width, string_address name)
{
        positive counted[5] = {lines, words, bytes, bytes, longest};
        bool wanted[5] = {wc_want_lines, wc_want_words, wc_want_chars,
                          wc_want_bytes, wc_want_longest};
        bool leading = true;

        for (b32 column = 0; column < 5; column++)
        {
                if (!wanted[column])
                        continue;

                if (!leading)
                        text_put_character(' ');

                positive_to_padded(text_put, counted[column], width, ' ', 0);
                leading = false;
        }

        if (name)
        {
                text_put_character(' ');
                text_put_string(name);
        }

        text_put_character('\n');
}

static b32 text_wc()
{
        file_taking taking = {
            .program = (string_address) "wc",
            .allowed = (string_address) "Lclmw",
            .valued = (string_address) "T",
            .longs = wc_longs,
            .operand = text_file_add,
        };

        text_begin("wc");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive flags = taking.flags;
        bool want_lines = (flags & FILE_FLAG('l')) != 0;
        bool want_words = (flags & FILE_FLAG('w')) != 0;
        bool want_bytes = (flags & FILE_FLAG('c')) != 0;
        bool want_chars = (flags & FILE_FLAG('m')) != 0;
        bool want_longest = (flags & FILE_FLAG('L')) != 0;
        positive total_mode = WC_TOTAL_AUTO;
        positive total_lines = 0, total_words = 0, total_bytes = 0, total_longest = 0;
        positive width = 1;
        positive known = 0;
        bool unknown = false;

        if (!want_lines && !want_words && !want_bytes && !want_chars && !want_longest)
        {
                want_lines = true;
                want_words = true;
                want_bytes = true;
        }

        if ((flags & FILE_FLAG('T')) &&
            !wc_total_of(file_option_value(address_of taking, 'T'),
                         address_of total_mode))
                return text_refuse(file_option_value(address_of taking, 'T'),
                                   "invalid argument", 1);

        b32 selected = (b32)want_lines + (b32)want_words + (b32)want_bytes +
                       (b32)want_chars + (b32)want_longest;
        b32 inputs = text_input_count();

        wc_want_lines = want_lines;
        wc_want_words = want_words;
        wc_want_bytes = want_bytes;
        wc_want_chars = want_chars;
        wc_want_longest = want_longest;

        if (total_mode != WC_TOTAL_ONLY && (selected > 1 || inputs > 1))
        {
                for (b32 i = 0; i < inputs; i++)
                {
                        string_address name = text_file_name(i);
                        positive size = 0;

                        if (!name || (name[0] == '-' && !name[1]))
                        {
                                if (!text_regular_size(0, address_of size))
                                        unknown = true;
                                else
                                        known += size;

                                continue;
                        }

                        file_facts facts;

                        if (!file_look_at(name, address_of facts))
                                continue;

                        if ((facts.mode & MODE_FORMAT) != MODE_FILE)
                        {
                                unknown = true;
                                continue;
                        }

                        if (facts.blocks)
                        {
                                known += facts.size;
                                continue;
                        }

                        bipolar probe = text_open_handle(name, FILE_READ, 0);

                        if (probe < 0)
                                continue;

                        if (!text_regular_size((positive)probe, address_of size))
                                unknown = true;
                        else
                                known += size;

                        system_close(probe);
                }

                if (unknown)
                {
                        width = 7;
                }
                else
                        width = positive_digits(known);
        }

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_file_name(i);
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
                                bipolar at = system_seek(text_input.handle, 0, 1);

                                if (at >= 0 && (positive)at <= size)
                                {
                                        bytes = size - (positive)at;
                                        system_seek(text_input.handle, 0, 2);
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

                                Word counting has its own stateful assembly
                                pass now, so every combination without -L can
                                stay in the narrow counters. The general loop
                                remains only where terminal column width is
                                genuinely part of the answer.
                        */
                        if (!want_longest)
                        {
                                if (want_lines)
                                        lines += memory_count(at, left, '\n');

                                if (want_words)
                                {
                                        positive2 counted_words =
                                            memory_count_words(at, left, inside);

                                        words += counted_words.x;
                                        inside = (bool)counted_words.y;
                                }

                                text_input.position = text_input.filled;
                                continue;
                        }

                        for (positive c = 0; c < left; c++)
                        {
                                p8 character = at[c];

                                if (want_lines && character == '\n')
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

                                if (want_words)
                                {
                                        if (byte_is_space(character))
                                        {
                                                inside = false;
                                        }
                                        else if (!inside)
                                        {
                                                inside = true;
                                                words++;
                                        }
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

                if (total_mode != WC_TOTAL_ONLY)
                        wc_row(lines, words, bytes, longest, width, name);
        }

        bool total = total_mode == WC_TOTAL_ALWAYS ||
                     total_mode == WC_TOTAL_ONLY ||
                     (total_mode == WC_TOTAL_AUTO && text_files_count > 1);

        if (total)
        {
                // The total of the longest lines is the longest of them, not
                // their sum, which is the one column here that does not add up.
                wc_row(total_lines, total_words, total_bytes, total_longest,
                       total_mode == WC_TOTAL_ONLY ? 1 : width,
                       total_mode == WC_TOTAL_ONLY ? null
                                                   : (string_address) "total");
        }

        return text_done(text_status);
}

// sum ----------------------------------------------------------------

static const file_long sum_longs[] = {
    {(string_address) "sysv", 's'},
    {null, 0},
};

static fn sum_output(p32 checksum, p64 bytes, string_address name,
                     bool named, bool sysv)
{
        positive block = sysv ? 512 : 1024;
        p64 blocks = bytes / block + (bytes % block != 0);

        if (sysv)
                positive_to_string(text_put, checksum);
        else
                positive_to_padded(text_put, checksum, 5, '0', 0);

        text_put_character(' ');

        if (sysv)
                positive_to_string(text_put, blocks);
        else
                positive_to_padded(text_put, blocks, 5, ' ', 0);

        if (named)
        {
                text_put_character(' ');
                text_put_string(name);
        }

        text_put_character('\n');
}

static b32 text_sum()
{
        p8 sum_option = 0;
        file_supersede supersedes[] = {
            {(string_address) "rs", address_of sum_option},
            {null, null},
        };
        file_taking taking = {
            .program = (string_address) "sum",
            .allowed = (string_address) "rs",
            .valued = (string_address) "",
            .longs = sum_longs,
            .operand = text_file_add,
            .supersedes = supersedes,
        };

        text_begin("sum");

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        bool sysv = sum_option == 's';
        bool named = text_files_count != 0;
        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_file_name(i);

                if (!text_open(name))
                        continue;

                p32 checksum = 0;
                p64 bytes = 0;

                while (text_fill())
                {
                        p8 address_to at = text_input.buffer + text_input.position;
                        positive length = text_input.filled - text_input.position;

                        if (sysv)
                                checksum += memory_sum_bytes(at, length);
                        else
                                checksum = memory_checksum_bsd16(at, length,
                                                                 checksum);

                        bytes += length;
                        text_input.position = text_input.filled;
                }

                text_close();

                if (text_input.failed)
                        continue;

                if (sysv)
                {
                        p32 folded = (checksum & 0xffff) + (checksum >> 16);

                        checksum = (folded & 0xffff) + (folded >> 16);
                }

                sum_output(checksum, bytes,
                           name ? name : (string_address) "-", named, sysv);
        }

        return text_done(text_status);
}

// tac ----------------------------------------------------------------

typedef byte_store tac_buffer;

static bool tac_read(tac_buffer address_to buffer, string_address name)
{
        buffer->used = 0;
        bool failed = false;

        if (!text_open(name))
                return false;

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive length = text_input.filled - text_input.position;

                if (length > positive_max - buffer->used ||
                    !byte_store_reserve(buffer, buffer->used + length,
                                        TEXT_READ_MAX))
                {
                        text_error(name, "input too large");
                        text_status = 1;
                        text_input.finished = true;
                        failed = true;
                        break;
                }

                memory_copy(buffer->bytes + buffer->used, at, length);
                buffer->used += length;
                text_input.position = text_input.filled;
        }

        bool okay = !text_input.failed && !failed;
        text_close();
        return okay;
}

static inline INLINE fn
tac_emit_reverse_piece(p8 address_to data, positive start, positive stop,
                       bool before, positive address_to past,
                       bool address_to first)
{
        if (before)
        {
                text_put(data + start, address_to past - start);
                address_to past = start;
        }
        else
        {
                if (!address_to first || stop != address_to past)
                        text_put(data + stop, address_to past - stop);

                address_to past = stop;
                address_to first = false;
        }
}

static fn tac_literal(p8 address_to data, positive length,
                      p8 address_to separator, positive separator_length,
                      bool before)
{
        positive past = length;
        positive cutoff = length;
        bool first = true;

        while (cutoff >= separator_length)
        {
                positive candidates = cutoff - separator_length + 1;
                p8 address_to found;

                for (;;)
                {
                        found = memory_last_of(data, separator[0], candidates);

                        if (!found || !memory_compare(found, separator,
                                                      separator_length))
                                break;

                        candidates = (positive)(found - data);
                }

                if (!found)
                        break;

                positive start = (positive)(found - data);
                positive stop = start + separator_length;

                tac_emit_reverse_piece(data, start, stop, before,
                                       address_of past, address_of first);

                cutoff = start;
        }

        if (past)
                text_put(data, past);
}

static fn tac_regex(p8 address_to data, positive length, bool before)
{
        positive past = length;
        positive cutoff = length;
        bool first = true;

        while (cutoff)
        {
                positive start = cutoff;
                bool found;

                do
                {
                        start--;
                        found = regex_match_longest(data, cutoff, start);
                }
                while (start && !found);

                if (!found)
                        break;

                positive stop = regex_slots[1];

                tac_emit_reverse_piece(data, start, stop, before,
                                       address_of past, address_of first);

                cutoff = start;
        }

        if (past)
                text_put(data, past);
}

static const file_long tac_longs[] = {
    {(string_address) "before", 'b'},
    {(string_address) "regex", 'r'},
    {(string_address) "separator", 's'},
    {null, 0},
};

static b32 text_tac()
{
        file_taking taking = {
            .program = (string_address) "tac",
            .allowed = (string_address) "brs",
            .valued = (string_address) "s",
            .longs = tac_longs,
            .operand = text_file_add,
        };
        tac_buffer input = {0};

        text_begin("tac");

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        string_address separator = file_option_value(address_of taking, 's');
        bool regex = (taking.flags & FILE_FLAG('r')) != 0;
        bool before = (taking.flags & FILE_FLAG('b')) != 0;
        p8 zero = 0;

        if (!separator)
                separator = (string_address) "\n";

        positive separator_length = string_length(separator);

        if (regex)
        {
                if (!separator_length)
                {
                        text_error(null, "separator cannot be empty");
                        return text_done(1);
                }

                if (!regex_compile(separator, false, false, false,
                                   REGEX_POLICY_TAC))
                {
                        text_error(null, "invalid regular expression");
                        return text_done(1);
                }
        }
        else if (!separator_length)
        {
                separator = address_of zero;
                separator_length = 1;
        }

        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs; i++)
                if (tac_read(address_of input, text_file_name(i)))
                {
                        if (regex)
                                tac_regex(input.bytes, input.used, before);
                        else
                                tac_literal(input.bytes, input.used, separator,
                                            separator_length, before);
                }

        byte_store_release(address_of input);
        return text_done(text_status);
}

// util-linux's rev, not coreutils': -0 rather than -z, and no -q or -v.
static const file_long rev_longs[] = {
    {(string_address) "zero", '0'},
    {null, 0},
};

static b32 text_rev()
{
        file_taking taking = {
            .program = (string_address) "rev",
            .allowed = (string_address) "0",
            .valued = (string_address) "",
            .longs = rev_longs,
            .operand = text_file_add,
        };

        text_begin("rev");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        if (taking.flags & FILE_FLAG('0'))
                text_delimiter = '\0';

        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
                        continue;

                while (text_line_next())
                {
                        memory_reverse(text_line, text_line_length);
                        text_put(text_line, text_line_length);

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

/*
        Whatever is left of the input, held whole in the arena, for the byte
        counts head and tail can only answer once a pipe has ended.

        One arena object retaken at its full length after every fill, the way
        text_arena_read_all grows, rather than one take per fill: a take is
        rounded up to sixteen bytes and a pipe hands over runs of any length,
        so the pieces would not sit end to end and the gap between two of
        them would be printed as part of the answer. Null is the arena
        refusing, which it has already said aloud.
*/
static p8 address_to text_arena_hold_rest(positive address_to have)
{
        p8 address_to held = (p8 address_to)text_arena_take(0);
        positive mark = text_arena_used;

        address_to have = 0;
        if (!held)
                return null;

        while (text_fill())
        {
                positive left = text_input.filled - text_input.position;

                text_arena_used = mark;
                if (!text_arena_take(address_to have + left))
                        return null;

                memory_copy(held + address_to have,
                            text_input.buffer + text_input.position, left);
                address_to have += left;
                text_input.position = text_input.filled;
        }

        return held;
}

static bool text_read_at(positive handle, positive offset, p8 address_to into, positive want)
{
        positive have = 0;

        system_seek(handle, offset, FILE_SEEK_SET);

        while (have < want)
        {
                bipolar got = system_read_retry(handle, into + have, want - have);

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
/*
        Where the input stands, as the tool sees it: the descriptor's offset
        less whatever the reader holds unread. A tool that reads standard
        input after another command on the same file starts where that one
        stopped, so this is the floor under head -n -N and tail, which used
        to count from the file's first byte.
*/
static positive text_stream_floor()
{
        bipolar at = system_seek(text_input.handle, 0, FILE_SEEK_CUR);
        positive held = text_input.filled - text_input.position;

        return at > 0 && (positive)at > held ? (positive)at - held : 0;
}

static positive text_tail_start(positive handle, positive size, positive count,
                                positive floor)
{
        p8 window[TEXT_READ_MAX];
        positive at = size;
        positive found = 0;

        if (!count)
                return size;

        while (at > floor)
        {
                positive take = at - floor < TEXT_READ_MAX ? at - floor
                                                           : TEXT_READ_MAX;

                at -= take;

                if (!text_read_at(handle, at, window, take))
                        return 0;

                positive usable = take;

                if (at + take == size && usable &&
                    window[usable - 1] == text_delimiter)
                        usable--;

                positive have = memory_count(window, usable, text_delimiter);

                if (found + have < count)
                {
                        found += have;
                        continue;
                }

                positive need = count - found;
                positive limit = usable;

                while (need)
                {
                        p8 address_to hit =
                            memory_last_of(window, (b8)text_delimiter, limit);

                        if (!hit)
                                break;

                        limit = (positive)(hit - window);
                        need--;

                        if (!need)
                                return at + limit + 1;
                }
        }

        return floor;
}

static fn text_stream_count(positive left)
{
        while (left && text_fill())
        {
                positive have = text_input.filled - text_input.position;
                positive take = have < left ? have : left;

                text_put(text_input.buffer + text_input.position, take);
                text_input.position += take;
                left -= take;
        }
}

static fn text_stream_seek(positive start)
{
        system_seek(text_input.handle, start, FILE_SEEK_SET);
        text_input.filled = 0;
        text_input.position = 0;
        text_input.finished = false;
}

static fn text_stream_from(positive start)
{
        text_stream_seek(start);
        text_put_rest();
}

// Everything from start up to but not including stop, which is where head
// stops when the count it was given was counted from the end.
static fn text_stream_span(positive start, positive stop)
{
        text_stream_seek(start);
        text_stream_count(stop > start ? stop - start : 0);
}

static const file_long head_longs[] = {
    {(string_address) "bytes", 'c'},
    {(string_address) "lines", 'n'},
    {(string_address) "quiet", 'q'},
    {(string_address) "silent", 'q'},
    {(string_address) "verbose", 'v'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

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
                positive floor = text_stream_floor();

                text_stream_span(floor,
                                 by_bytes ? (count < size - floor ? size - count
                                                                  : floor)
                                          : text_tail_start(text_input.handle,
                                                            size, count, floor));
                return;
        }

        if (!text_lines_ready())
                return;

        if (by_bytes)
        {
                positive have;
                p8 address_to held = text_arena_hold_rest(address_of have);

                if (!held)
                        return;

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

/*
        The count head and tail were given, and whether it carried the sign
        that means something other than a plain count.

        Both tools accept either sign and let one of the two change what the
        count means: head -n -5 leaves five off the end, tail -n +5 starts at
        the fifth line rather than ending at it. The other sign is decoration
        and is stepped over. Which of the two is the meaningful one is the
        whole difference between the two tools here, so it arrives as an
        argument rather than as a second copy of the parser.

        No option at all leaves the caller's default standing.
*/
static bool text_count_option(string_address said, p8 marked,
                              bool address_to special, positive address_to count)
{
        if (!said)
                return true;

        if (said[0] == marked)
        {
                address_to special = true;
                said++;
        }
        else if (said[0] == '+' || said[0] == '-')
                said++;

        if (string_digits_exact(said, count))
                return true;

        text_error(null, "invalid number of lines");
        return false;
}

static b32 text_head()
{
        file_taking taking = {
            .program = (string_address) "head",
            .allowed = (string_address) "cnqvz",
            .valued = (string_address) "cn",
            .longs = head_longs,
            .operand = text_file_add,
            // head -5 is head -n 5, and the digits are the count.
            .digits = 'n',
        };

        text_begin("head");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive count = 10;
        bool by_bytes = taking.last == 'c';
        bool from_end = false;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool loud = (taking.flags & FILE_FLAG('v')) != 0;
        string_address said = file_option_value(address_of taking,
                                                by_bytes ? 'c' : 'n');

        if (taking.flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        // A count written with a minus names what to leave off the end rather
        // than what to take from the front. A plus is an explicit ordinary
        // count for head, unlike tail where it means a starting position.
        if (!text_count_option(said, '-', address_of from_end, address_of count))
                return text_done(1);

        b32 inputs = text_input_count();
        bool headers = (text_files_count > 1 || loud) && !quiet;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
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
                        text_stream_count(count);
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
// --pid and --max-unchanged-stats are waited on rather than read, and --retry
// and --debug say nothing about the bytes. P and R are letters tail has not
// got, so the words reach a bit of the flag word and -P stays a mistake.

static const file_long tail_longs[] = {
    {(string_address) "bytes", 'c'},
    {(string_address) "lines", 'n'},
    {(string_address) "quiet", 'q'},
    {(string_address) "silent", 'q'},
    {(string_address) "verbose", 'v'},
    {(string_address) "follow", 'F'},
    {(string_address) "retry", 'R'},
    {(string_address) "pid", 'P'},
    {(string_address) "sleep-interval", 's'},
    {(string_address) "max-unchanged-stats", 'P'},
    {(string_address) "debug", 'R'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

static b32 text_tail()
{
        file_taking taking = {
            .program = (string_address) "tail",
            // -f waits for more to be written, which is a wait this does not
            // do: the file is read to its end and that is where GNU would
            // still be sitting. -s is how long it would have waited.
            .allowed = (string_address) "cfnqsvz",
            .valued = (string_address) "Pcns",
            .optional = (string_address) "F",
            .longs = tail_longs,
            .operand = text_file_add,
            .digits = 'n',
        };

        text_begin("tail");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive count = 10;
        bool by_bytes = taking.last == 'c';
        bool from_start = false;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool loud = (taking.flags & FILE_FLAG('v')) != 0;
        string_address said = file_option_value(address_of taking,
                                                by_bytes ? 'c' : 'n');

        if (taking.flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        if (!text_count_option(said, '+', address_of from_start,
                               address_of count))
                return text_done(1);

        b32 inputs = text_input_count();
        bool headers = (text_files_count > 1 || loud) && !quiet;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
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
                        positive floor = text_stream_floor();

                        text_stream_from(
                            by_bytes ? (count < size - floor ? size - count
                                                             : floor)
                                     : text_tail_start(text_input.handle, size,
                                                       count, floor));
                        text_close();
                        continue;
                }

                if (by_bytes)
                {
                        // Bytes rather than lines, so the whole input is held
                        // and the tail of it handed back.
                        if (!text_lines_ready())
                                return text_done(1);

                        positive have;
                        p8 address_to held = text_arena_hold_rest(address_of have);

                        if (!held)
                                return text_done(1);

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
static const file_long tee_longs[] = {
    {(string_address) "append", 'a'},
    {(string_address) "ignore-interrupts", 'i'},
    {(string_address) "output-error", 'O'},
    {null, 0},
};

static b32 text_tee()
{
        positive address_to handles = null;
        positive handle_count = 0;
        file_taking taking = {
            .program = (string_address) "tee",
            .allowed = (string_address) "aip",
            .valued = (string_address) "",
            // --output-error names a kind of failure to go on through, and
            // the word it carries is taken and dropped like -i and -p are. O
            // is a letter tee has not got, so -p stays a plain flag.
            .optional = (string_address) "O",
            .longs = tee_longs,
            .operand = text_file_add,
        };

        text_begin("tee");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        bool append = (taking.flags & FILE_FLAG('a')) != 0;

        if (text_files_count)
        {
                if (text_files_count > positive_max / sizeof(positive))
                        return text_error(null, "too many operands"), text_done(1);

                positive mapped =
                    (positive)memory(text_files_count * sizeof(positive));

                if (!mapped || mapped >= (positive)-4095)
                        return text_error(null, "too many operands"), text_done(1);

                handles = (positive address_to)mapped;
        }

        for (positive i = 0; i < text_files_count; i++)
        {
                string_address name = program_argument(text_files[i]);
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
        {
                if (handles)
                        system_call_2(syscall(munmap), (positive)handles,
                                      text_files_count * sizeof(positive));

                return text_done(1);
        }

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;

                if (system_write_all(1, at, left) != left)
                        text_out_failed = true;

                for (positive i = 0; i < handle_count; i++)
                        if (system_write_all(handles[i], at, left) != left)
                                text_status = 1;

                text_input.position = text_input.filled;
        }

        for (positive i = 0; i < handle_count; i++)
                if (system_close(handles[i]) < 0)
                        text_status = 1;

        if (handles)
                system_call_2(syscall(munmap), (positive)handles,
                              text_files_count * sizeof(positive));

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
static const file_long nl_longs[] = {
    {(string_address) "body-numbering", 'b'},
    {(string_address) "section-delimiter", 'd'},
    {(string_address) "footer-numbering", 'f'},
    {(string_address) "header-numbering", 'h'},
    {(string_address) "line-increment", 'i'},
    {(string_address) "join-blank-lines", 'l'},
    {(string_address) "number-format", 'n'},
    {(string_address) "no-renumber", 'p'},
    {(string_address) "number-separator", 's'},
    {(string_address) "starting-line-number", 'v'},
    {(string_address) "number-width", 'w'},
    {null, 0},
};

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
        file_taking taking = {
            .program = (string_address) "nl",
            .allowed = (string_address) "bdfhilnpsvw",
            .valued = (string_address) "bdfhilnsvw",
            .longs = nl_longs,
            .operand = text_file_add,
        };

        text_begin("nl");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

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
        bool keep_counting = (taking.flags & FILE_FLAG('p')) != 0;
        string_address said;

        // Header, body and footer are numbered by their own rule, and a style
        // of p is that rule written as a pattern.
        for (positive k = 0; k < 3; k++)
        {
                said = file_option_value(address_of taking,
                                         k == 0 ? 'h' : (k == 1 ? 'b' : 'f'));

                if (!said)
                        continue;

                styles[k] = said[0];

                if (said[0] != 'p')
                        continue;

                if (pattern_count >= 3 ||
                    !regex_compile(said + 1, false, false, false,
                                   REGEX_POLICY_DEFAULT))
                        return text_refuse(said + 1,
                                           "invalid regular expression", 1);

                regex_keep(nl_patterns + pattern_count);
                patterns[k] = pattern_count++;
        }

        said = file_option_value(address_of taking, 'd');

        if (said)
        {
                // One character given leaves the second as it was, so nl -d @
                // looks for @: and not for @@.
                delimiter[0] = said[0];

                if (said[0] && said[1])
                        delimiter[1] = said[1];
        }

        said = file_option_value(address_of taking, 'n');

        if (said)
        {
                justify = said[0];
                zeros = said[0] == 'r' && said[1] == 'z';
        }

        if (taking.flags & FILE_FLAG('s'))
                separator = file_option_value(address_of taking, 's');

        if (taking.flags & FILE_FLAG('w'))
                string_digits_exact(file_option_value(address_of taking, 'w'),
                                    address_of width);

        if (taking.flags & FILE_FLAG('v'))
                string_digits_exact(file_option_value(address_of taking, 'v'),
                                    address_of number);

        if (taking.flags & FILE_FLAG('i'))
                string_digits_exact(file_option_value(address_of taking, 'i'),
                                    address_of step);

        if (taking.flags & FILE_FLAG('l'))
                string_digits_exact(file_option_value(address_of taking, 'l'),
                                    address_of join);

        if (!join)
                join = 1;

        b32 inputs = text_input_count();
        positive separator_length = string_length(separator);

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
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
                                        positive_to_base_field(
                                            text_put, number, 10, width, -1,
                                            (positive)1 << 27);
                                else if (zeros)
                                {
                                        positive_to_padded(text_put, number, width, '0', 0);
                                }
                                else
                                {
                                        positive_to_padded(text_put, number, width, ' ', 0);
                                }

                                text_put(separator, separator_length);
                                number += step;
                        }
                        else
                        {
                                // The columns stay, so an unnumbered line
                                // lines up under a numbered one.
                                writer_fill(text_put, width + separator_length, ' ');
                        }

                        text_put_line();

                        // nl produces complete display lines even when the
                        // final input line had no terminator. The other line
                        // filters preserve that byte-level distinction.
                        if (!text_line_ended)
                                text_put_character('\n');
                }

                text_close();
        }

        return text_done(text_status);
}

static const file_long fold_longs[] = {
    {(string_address) "bytes", 'b'},
    {(string_address) "characters", 'c'},
    {(string_address) "spaces", 's'},
    {(string_address) "width", 'w'},
    {null, 0},
};

/*
        Tabs are display positions, not byte counts.  expand and unexpand
        therefore share one description of the stops and one streaming
        column machine.  The only held state for unexpand is the pair of
        columns bounding a run of spaces; input and output still use the
        common 64 KiB reader and writer above.

        GNU's LIST grammar has two useful extensions.  /N repeats at the
        multiples of N after the explicit stops, while +N repeats relative
        to the last explicit stop.  Several -t options contribute to the
        same list, so option parsing appends positions instead of retaining
        only file_taking's final value.
*/
#define TEXT_TAB_STOP_MAX 1024

static positive text_tab_stops[TEXT_TAB_STOP_MAX];
static positive text_tab_stop_count;
static positive text_tab_repeat;
static bool text_tab_repeat_relative;
static bool text_tab_repeat_said;
static bool text_tab_custom;
static bool text_tab_option_seen;

static p8 text_tab_expand_span[256];
static p8 text_tab_unexpand_span[256];
static p8 text_tab_space_span[256];

static fn text_tab_reset()
{
        text_tab_stop_count = 0;
        text_tab_repeat = 8;
        text_tab_repeat_relative = false;
        text_tab_repeat_said = false;
        text_tab_custom = false;
        text_tab_option_seen = false;
}

static bool text_tab_number(string_address at, positive address_to used,
                            positive address_to made)
{
        positive value = 0;
        positive digits = 0;

        while (byte_is_digit(at[digits]))
        {
                positive digit = at[digits++] - '0';

                if (value > (positive_max - digit) / 10)
                        return false;

                value = value * 10 + digit;
        }

        if (!digits)
                return false;

        address_to used = digits;
        address_to made = value;
        return true;
}

static bool text_tab_parse(string_address list)
{
        positive at = 0;
        bool any = false;

        if (!text_tab_custom)
        {
                text_tab_custom = true;
                text_tab_repeat = 0;
        }

        while (list[at])
        {
                while (list[at] == ',' || byte_is_space(list[at]))
                        at++;

                if (!list[at])
                        break;

                p8 prefix = 0;

                if (list[at] == '/' || list[at] == '+')
                        prefix = list[at++];

                positive used;
                positive value;

                if (!text_tab_number(list + at, address_of used,
                                     address_of value))
                {
                        text_error(list, "invalid tab stops");
                        return false;
                }

                at += used;

                if (list[at] && list[at] != ',' && !byte_is_space(list[at]))
                {
                        text_error(list + at, "invalid tab stops");
                        return false;
                }

                any = true;

                if (prefix)
                {
                        positive after = at;

                        while (list[after] == ',' || byte_is_space(list[after]))
                                after++;

                        if (list[after])
                        {
                                text_error(list, "tab repeat must be last");
                                return false;
                        }

                        text_tab_repeat = value;
                        text_tab_repeat_relative = prefix == '+';
                        text_tab_repeat_said = true;
                        at = after;
                        continue;
                }

                if (!value)
                {
                        text_error(list, "tab stop cannot be zero");
                        return false;
                }

                if (text_tab_stop_count &&
                    value <= text_tab_stops[text_tab_stop_count - 1])
                {
                        text_error(list, "tab stops must be ascending");
                        return false;
                }

                if (text_tab_stop_count == TEXT_TAB_STOP_MAX)
                {
                        text_error(list, "too many tab stops");
                        return false;
                }

                text_tab_stops[text_tab_stop_count++] = value;
        }

        if (!any)
        {
                text_error(list, "empty tab list");
                return false;
        }

        return true;
}

static bool text_tab_seen(p8 letter, string_address value)
{
        if (letter != 't')
                return true;

        text_tab_option_seen = true;
        return text_tab_parse(value);
}

static fn text_tab_finish_options()
{
        /* A lone ordinary number is a spacing, not a finite one-stop list. */
        if (text_tab_custom && text_tab_stop_count == 1 &&
            !text_tab_repeat_said)
        {
                text_tab_repeat = text_tab_stops[0];
                text_tab_stop_count = 0;
        }
}

/* The first stop strictly after column, and whether it was explicitly named. */
static bool text_tab_next(positive column, positive address_to next,
                          bool address_to explicit)
{
        positive low = 0;
        positive high = text_tab_stop_count;

        while (low < high)
        {
                positive middle = low + (high - low) / 2;

                if (text_tab_stops[middle] <= column)
                        low = middle + 1;
                else
                        high = middle;
        }

        if (low < text_tab_stop_count)
        {
                address_to next = text_tab_stops[low];
                address_to explicit = true;
                return true;
        }

        if (!text_tab_repeat)
                return false;

        positive base = text_tab_repeat_relative && text_tab_stop_count
                            ? text_tab_stops[text_tab_stop_count - 1]
                            : 0;
        positive distance = column >= base ? column - base : 0;
        positive steps = distance / text_tab_repeat + 1;

        if (steps > (positive_max - base) / text_tab_repeat)
                return false;

        address_to next = base + steps * text_tab_repeat;
        address_to explicit = false;
        return true;
}

static fn text_tab_repeat_character(p8 character, positive count)
{
        while (count)
        {
                positive take = count > TEXT_OUT_MAX ? TEXT_OUT_MAX : count;
                p8 address_to out = text_reserve(take);

                if (!out)
                        return;

                memory_fill(out, character, take);
                count -= take;
        }
}

/* GNU delays blanks until it knows whether their first byte belongs at a tab
   stop. In the C byte locale they are all spaces except possibly that first
   byte, so a count and one bit replace its allocated pending-byte array. */
static fn text_unexpand_pending(positive count, bool first_tab)
{
        if (!count)
                return;

        if (first_tab)
        {
                text_put_character('\t');
                count--;
        }

        text_tab_repeat_character(' ', count);
}

static fn text_tab_sets()
{
        if (text_tab_space_span[' '])
                return;

        memory_fill(text_tab_expand_span, 1, sizeof(text_tab_expand_span));
        memory_fill(text_tab_unexpand_span, 1, sizeof(text_tab_unexpand_span));
        text_tab_space_span[' '] = 1;

        text_tab_expand_span['\t'] = 0;
        text_tab_expand_span['\b'] = 0;
        text_tab_expand_span['\n'] = 0;
        memory_copy_apart(text_tab_unexpand_span, text_tab_expand_span,
                          sizeof(text_tab_expand_span));
        text_tab_unexpand_span[' '] = 0;
}

static fn text_tab_transform(bool unexpand, bool initial_only)
{
        positive column = 0;
        positive pending = 0;
        bool pending_first_tab = false;
        bool one_blank_before_stop = false;
        bool previous_blank = true;
        bool convert = true;
        b32 inputs = text_input_count();

        text_tab_sets();

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
                        continue;

                while (text_fill())
                {
                        p8 address_to data = text_input.buffer + text_input.position;
                        positive left = text_input.filled - text_input.position;
                        positive at = 0;

                        while (at < left)
                        {
                                if (!convert)
                                {
                                        string_address newline = memory_first_of(
                                            data + at, '\n', left - at);
                                        positive take = newline
                                                            ? (positive)(newline - data - at) + 1
                                                            : left - at;

                                        text_put(data + at, take);
                                        at += take;

                                        if (newline)
                                        {
                                                column = 0;
                                                pending = 0;
                                                pending_first_tab = false;
                                                one_blank_before_stop = false;
                                                previous_blank = true;
                                                convert = true;
                                        }

                                        continue;
                                }

                                if (unexpand)
                                {
                                        p8 character = data[at++];
                                        bool blank = character == ' ' ||
                                                     character == '\t';
                                        bool suppress = false;

                                        if (blank)
                                        {
                                                positive stop;
                                                bool explicit;
                                                bool have = text_tab_next(
                                                    column, address_of stop,
                                                    address_of explicit);

                                                if (!have)
                                                        convert = false;
                                                else if (character == '\t')
                                                {
                                                        column = stop;

                                                        if (pending)
                                                                pending_first_tab = true;

                                                        pending = one_blank_before_stop;

                                                        if (!pending)
                                                                pending_first_tab = false;
                                                }
                                                else
                                                {
                                                        if (column != positive_max)
                                                                column++;

                                                        if (!(previous_blank &&
                                                              column >= stop))
                                                        {
                                                                if (column == stop)
                                                                        one_blank_before_stop =
                                                                            true;

                                                                pending++;
                                                                previous_blank = true;
                                                                continue;
                                                        }

                                                        text_put_character('\t');
                                                        pending_first_tab = true;
                                                        pending = one_blank_before_stop;

                                                        if (!pending)
                                                                pending_first_tab = false;
                                                        suppress = true;
                                                }
                                        }
                                        else if (character == '\b')
                                                column = column ? column - 1 : 0;
                                        else
                                        {
                                                if (column != positive_max)
                                                        column++;
                                        }

                                        if (pending)
                                        {
                                                if (pending > 1 &&
                                                    one_blank_before_stop)
                                                        pending_first_tab = true;

                                                text_unexpand_pending(
                                                    pending, pending_first_tab);
                                                pending = 0;
                                                pending_first_tab = false;
                                                one_blank_before_stop = false;
                                        }

                                        previous_blank = blank;

                                        if (initial_only && !blank)
                                                convert = false;

                                        if (!suppress)
                                                text_put_character(character);

                                        if (character == '\n')
                                        {
                                                column = 0;
                                                previous_blank = true;
                                                convert = true;
                                        }

                                        continue;
                                }

                                if (initial_only && data[at] == ' ')
                                {
                                        positive run = string_span_max(
                                            data + at, left - at,
                                            text_tab_space_span);

                                        text_put(data + at, run);
                                        column += run;
                                        at += run;
                                        continue;
                                }

                                p8 character = data[at];

                                if (initial_only && character != '\t' &&
                                    character != '\n')
                                {
                                        convert = false;
                                        continue;
                                }

                                positive run = string_span_max(
                                    data + at, left - at,
                                    text_tab_expand_span);

                                if (run)
                                {
                                        text_put(data + at, run);
                                        column = column > positive_max - run
                                                     ? positive_max
                                                     : column + run;
                                        at += run;
                                        continue;
                                }

                                character = data[at++];

                                if (character == '\t')
                                {
                                        positive stop;
                                        bool explicit;
                                        bool have = text_tab_next(
                                            column, address_of stop,
                                            address_of explicit);

                                        positive after = have ? stop
                                                              : column +
                                                                    (column != positive_max);
                                        text_tab_repeat_character(' ',
                                                                  after - column);
                                        column = after;

                                        continue;
                                }

                                text_put_character(character);

                                if (character == '\n')
                                {
                                        column = 0;
                                        convert = true;
                                }
                                else if (character == '\b')
                                        column = column ? column - 1 : 0;
                        }

                        text_input.position = text_input.filled;
                }

                text_close();
        }

        if (unexpand && pending)
        {
                if (pending > 1 && one_blank_before_stop)
                        pending_first_tab = true;

                text_unexpand_pending(pending, pending_first_tab);
        }
}

static const file_long expand_longs[] = {
    {(string_address) "initial", 'i'},
    {(string_address) "tabs", 't'},
    {null, 0},
};

static const file_long unexpand_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "first-only", 'f'},
    {(string_address) "tabs", 't'},
    {null, 0},
};

static b32 text_expand()
{
        file_taking taking = {
            .program = (string_address) "expand",
            .allowed = (string_address) "it",
            .valued = (string_address) "t",
            .longs = expand_longs,
            .operand = text_file_add,
            .seen = text_tab_seen,
            .digits = 'T',
        };

        text_begin("expand");
        text_tab_reset();

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        if (taking.flags & FILE_FLAG('T'))
        {
                if (text_tab_option_seen)
                        return text_refuse(null,
                                           "cannot mix -N and --tabs syntax", 1);

                if (!text_tab_parse(file_option_value(address_of taking, 'T')))
                        return text_done(1);
        }

        text_tab_finish_options();
        text_tab_transform(false, (taking.flags & FILE_FLAG('i')) != 0);
        return text_done(text_status);
}

static b32 text_unexpand()
{
        file_taking taking = {
            .program = (string_address) "unexpand",
            .allowed = (string_address) "at",
            .valued = (string_address) "t",
            .longs = unexpand_longs,
            .operand = text_file_add,
            .seen = text_tab_seen,
            .digits = 'T',
        };

        text_begin("unexpand");
        text_tab_reset();

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        if (taking.flags & FILE_FLAG('T'))
        {
                if (text_tab_option_seen)
                        return text_refuse(null,
                                           "cannot mix -N and --tabs syntax", 1);

                if (!text_tab_parse(file_option_value(address_of taking, 'T')))
                        return text_done(1);
        }

        text_tab_finish_options();

        bool first = (taking.flags & FILE_FLAG('f')) != 0;
        bool all = !first && ((taking.flags & FILE_FLAG('a')) ||
                              text_tab_option_seen);

        text_tab_transform(true, !all);
        return text_done(text_status);
}

/*
        Paragraph filling.

        fmt is not a greedy fold: it assigns a cost to every legal break and
        chooses the least-ragged set for the whole paragraph.  The descriptors
        below retain that small dynamic-programming graph, while word bytes
        use relation_spill (already the shared one-record spill area) and input
        lines use text_record_hold.  It therefore adds neither another reader
        nor another line/output buffer.
*/
#define FMT_WIDTH_DEFAULT 75
/* Word separators are represented in descriptors rather than copied into the
   spill, so a one-byte word on each physical line is the true upper bound. */
#define FMT_WORD_MAX TEXT_LINE_MAX
#define FMT_COST_MAX ((bipolar)(positive_max >> 1))

typedef struct
{
        p8 address_to text;
        positive length;
        positive space;
        positive line_length;
        positive next_break;
        bipolar best_cost;
        bool paren;
        bool period;
        bool punct;
        bool final;
} fmt_word;

typedef struct
{
        p8 address_to at;
        positive length;
        bool ended;
        bool suitable;
        positive prefix_indent;
        positive indent;
        positive content;
} fmt_line;

static fmt_word address_to fmt_words;
static positive fmt_word_count;
static positive fmt_character_count;
static positive fmt_max_width;
static positive fmt_goal_width;
static string_address fmt_prefix;
static positive fmt_prefix_leading;
static positive fmt_prefix_length;
static positive fmt_prefix_full_length;
static positive fmt_prefix_indent;
static positive fmt_first_indent;
static positive fmt_other_indent;
static positive fmt_last_line_length;
static positive fmt_out_column;
static bool fmt_tabs;
static bool fmt_crown;
static bool fmt_tagged;
static bool fmt_split;
static bool fmt_uniform;
static bool fmt_failed;
static b8 fmt_word_bytes[STRING_SET_BYTES];

static fn fmt_put_space(positive count)
{
        positive target = fmt_out_column > positive_max - count
                              ? positive_max
                              : fmt_out_column + count;

        if (fmt_tabs)
        {
                positive tab_target = target / 8 * 8;

                if (fmt_out_column + (fmt_out_column != positive_max) <
                    tab_target)
                {
                        positive first = (fmt_out_column / 8 + 1) * 8;
                        positive tabs = 1 + (tab_target - first) / 8;

                        text_tab_repeat_character('\t', tabs);
                        fmt_out_column = tab_target;
                }
        }

        text_tab_repeat_character(' ', target - fmt_out_column);
        fmt_out_column = target;
}

static bool fmt_open_character(p8 value)
{
        return value == '(' || value == '[' || value == '\'' || value == '`' ||
               value == '"';
}

static bool fmt_close_character(p8 value)
{
        return value == ')' || value == ']' || value == '\'' || value == '"';
}

static bool fmt_period_character(p8 value)
{
        return value == '.' || value == '?' || value == '!';
}

static fn fmt_check_punctuation(fmt_word address_to word)
{
        p8 address_to start = word->text;
        positive finish = word->length - 1;

        word->paren = fmt_open_character(start[0]);
        word->punct = byte_is_punctuation(start[finish]);

        while (finish && fmt_close_character(start[finish]))
                finish--;

        word->period = fmt_period_character(start[finish]);
}

static positive fmt_blanks(p8 address_to line, positive length,
                           positive address_to at, positive column)
{
        while (address_to at < length)
        {
                p8 character = line[address_to at];

                if (character == ' ')
                        column++;
                else if (character == '\t')
                {
                        fmt_tabs = true;
                        column = (column / 8 + 1) * 8;
                }
                else
                        break;

                address_to at = address_to at + 1;
        }

        return column;
}

static fn fmt_analyze_line(fmt_line address_to line)
{
        positive at = 0;
        positive column = fmt_blanks(line->at, line->length, address_of at, 0);

        if (!fmt_prefix_length)
                line->prefix_indent = column < fmt_prefix_leading
                                          ? column
                                          : fmt_prefix_leading;
        else
        {
                line->prefix_indent = column;

                positive matched = 0;

                while (matched < fmt_prefix_length && at < line->length &&
                       line->at[at] == fmt_prefix[matched])
                {
                        matched++;
                        at++;
                        column++;
                }

                if (matched != fmt_prefix_length)
                {
                        line->indent = column;
                        line->content = at;
                        line->suitable = false;
                        return;
                }

                column = fmt_blanks(line->at, line->length, address_of at,
                                    column);
        }

        line->indent = column;
        line->content = at;
        line->suitable = at < line->length &&
                         line->prefix_indent >= fmt_prefix_leading &&
                         column >= line->prefix_indent + fmt_prefix_full_length;
}

static bool fmt_read_line(fmt_line address_to line)
{
        if (!text_line_next())
                return false;

        memory_copy_apart(text_record_hold, text_line, text_line_length);
        line->at = text_record_hold;
        line->length = text_line_length;
        line->ended = text_line_ended;
        line->suitable = false;
        line->prefix_indent = 0;
        line->indent = 0;
        line->content = 0;
        fmt_analyze_line(line);
        return true;
}

static bool fmt_add_word(p8 address_to at, positive length, positive space,
                         bool end_line)
{
        if (fmt_word_count == FMT_WORD_MAX ||
            length > TEXT_LINE_MAX - fmt_character_count)
        {
                text_error(null, "paragraph is too large");
                fmt_failed = true;
                return false;
        }

        fmt_word address_to word = fmt_words + fmt_word_count++;

        word->text = relation_spill + fmt_character_count;
        memory_copy_apart(word->text, at, length);
        fmt_character_count += length;
        word->length = length;
        word->space = space;
        word->line_length = 0;
        word->next_break = fmt_word_count;
        word->best_cost = 0;
        word->paren = false;
        word->period = false;
        word->punct = false;
        word->final = false;
        fmt_check_punctuation(word);
        word->final = word->period && (end_line || space > 1);

        if (end_line || fmt_uniform)
                word->space = word->final ? 2 : 1;

        return true;
}

static bool fmt_add_line(fmt_line address_to line)
{
        positive at = line->content;
        positive column = line->indent;

        while (at < line->length)
        {
                positive begin = at;

                at += string_span_max(line->at + at, line->length - at,
                                      fmt_word_bytes);

                /* A non-space separator outside the ordinary blank pair is
                   retained with the following word, matching fmt's byte-C
                   behavior instead of silently deleting input controls. */
                if (begin == at)
                        at++;

                positive length = at - begin;
                column += length;
                positive before = column;

                column = fmt_blanks(line->at, line->length, address_of at,
                                    column);
                positive space = column - before;
                bool end_line = at == line->length;

                if (!fmt_add_word(line->at + begin, length, space, end_line))
                        return false;
        }

        return true;
}

static bipolar fmt_square_cost(bipolar difference)
{
        bipolar amount = difference * 10;

        return amount * amount;
}

static bipolar fmt_base_cost(positive index)
{
        fmt_word address_to word = fmt_words + index;
        bipolar cost = 4900;

        if (index)
        {
                fmt_word address_to before = word - 1;

                if (before->period)
                        cost += before->final ? -2500 : 360000;
                else if (before->punct)
                        cost -= 1600;
                else if (index > 1 && (word - 2)->final)
                        cost += 40000 / (bipolar)(before->length + 2);
        }

        if (word->paren)
                cost -= 1600;
        else if (word->final)
                cost += 22500 / (bipolar)(word->length + 2);

        return cost;
}

static bipolar fmt_line_cost(positive next, positive length)
{
        if (next == fmt_word_count)
                return 0;

        bipolar cost = fmt_square_cost((bipolar)fmt_goal_width -
                                       (bipolar)length);

        if (fmt_words[next].next_break != fmt_word_count)
        {
                bipolar difference = (bipolar)length -
                                     (bipolar)fmt_words[next].line_length;

                cost += fmt_square_cost(difference) / 2;
        }

        return cost;
}

static fn fmt_choose_breaks()
{
        fmt_words[fmt_word_count].best_cost = 0;
        fmt_words[fmt_word_count].length = fmt_max_width;
        fmt_words[fmt_word_count].next_break = fmt_word_count;

        for (positive start = fmt_word_count; start;)
        {
                start--;
                bipolar best = FMT_COST_MAX;
                positive length = start ? fmt_other_indent : fmt_first_indent;
                positive next = start;

                length += fmt_words[next].length;

                do
                {
                        next++;
                        bipolar cost = fmt_line_cost(next, length) +
                                       fmt_words[next].best_cost;

                        if (!start && fmt_last_line_length)
                        {
                                bipolar difference = (bipolar)length -
                                                     (bipolar)fmt_last_line_length;

                                cost += fmt_square_cost(difference) / 2;
                        }

                        if (cost < best)
                        {
                                best = cost;
                                fmt_words[start].next_break = next;
                                fmt_words[start].line_length = length;
                        }

                        if (next == fmt_word_count)
                                break;

                        length += fmt_words[next - 1].space +
                                  fmt_words[next].length;
                }
                while (length <= fmt_max_width);

                fmt_words[start].best_cost = best + fmt_base_cost(start);
        }
}

static fn fmt_put_line(positive begin, positive indent)
{
        positive finish = fmt_words[begin].next_break;

        fmt_out_column = 0;
        fmt_put_space(fmt_prefix_indent);
        text_put(fmt_prefix, fmt_prefix_length);
        fmt_out_column += fmt_prefix_length;
        fmt_put_space(indent - fmt_out_column);

        for (positive at = begin; at < finish; at++)
        {
                text_put(fmt_words[at].text, fmt_words[at].length);
                fmt_out_column += fmt_words[at].length;

                if (at + 1 < finish)
                        fmt_put_space(fmt_words[at].space);
        }

        fmt_last_line_length = fmt_out_column;
        text_put_character('\n');
}

static fn fmt_put_paragraph()
{
        positive at = 0;

        fmt_put_line(at, fmt_first_indent);
        at = fmt_words[at].next_break;

        while (at < fmt_word_count)
        {
                fmt_put_line(at, fmt_other_indent);
                at = fmt_words[at].next_break;
        }
}

static fn fmt_other(bool same, positive next_indent)
{
        if (fmt_split)
                fmt_other_indent = fmt_first_indent;
        else if (fmt_crown)
                fmt_other_indent = same ? next_indent : fmt_first_indent;
        else if (fmt_tagged)
        {
                if (same && next_indent != fmt_first_indent)
                        fmt_other_indent = next_indent;
                else if (fmt_other_indent == fmt_first_indent)
                        fmt_other_indent = fmt_first_indent ? 0 : 3;
        }
        else
                fmt_other_indent = fmt_first_indent;
}

static bool fmt_same(fmt_line address_to line)
{
        return line->suitable && line->prefix_indent == fmt_prefix_indent;
}

static fn fmt_copy_line(fmt_line address_to line)
{
        fmt_out_column = 0;

        if (line->indent > line->prefix_indent ||
            line->content < line->length)
        {
                fmt_put_space(line->prefix_indent);

                positive prefix = 0;

                while (fmt_out_column != line->indent &&
                       prefix < fmt_prefix_length)
                {
                        text_put_character(fmt_prefix[prefix++]);
                        fmt_out_column++;
                }

                if (line->content < line->length)
                        fmt_put_space(line->indent - fmt_out_column);
                else if (!line->ended &&
                         line->indent >= line->prefix_indent +
                                             fmt_prefix_length)
                        text_put_character('\n');
        }

        text_put(line->at + line->content, line->length - line->content);

        if (line->ended)
                text_put_character('\n');
}

static fn fmt_file()
{
        fmt_line line;

        fmt_tabs = false;
        fmt_other_indent = 0;
        bool have = fmt_read_line(address_of line);

        while (have && !fmt_failed)
        {
                if (!line.suitable)
                {
                        fmt_copy_line(address_of line);
                        have = fmt_read_line(address_of line);
                        continue;
                }

                fmt_word_count = 0;
                fmt_character_count = 0;
                fmt_last_line_length = 0;
                fmt_prefix_indent = line.prefix_indent;
                fmt_first_indent = line.indent;

                if (!fmt_add_line(address_of line))
                        break;

                have = fmt_read_line(address_of line);
                bool same = have && fmt_same(address_of line);

                fmt_other(same, same ? line.indent : fmt_first_indent);

                if (!fmt_split)
                {
                        if (fmt_crown && same)
                        {
                                do
                                {
                                        if (!fmt_add_line(address_of line))
                                                break;

                                        have = fmt_read_line(address_of line);
                                }
                                while (have && fmt_same(address_of line) &&
                                       line.indent == fmt_other_indent);
                        }
                        else if (fmt_tagged && same &&
                                 line.indent != fmt_first_indent)
                        {
                                do
                                {
                                        if (!fmt_add_line(address_of line))
                                                break;

                                        have = fmt_read_line(address_of line);
                                }
                                while (have && fmt_same(address_of line) &&
                                       line.indent == fmt_other_indent);
                        }
                        else if (!fmt_crown && !fmt_tagged)
                        {
                                while (same && line.indent == fmt_other_indent)
                                {
                                        if (!fmt_add_line(address_of line))
                                                break;

                                        have = fmt_read_line(address_of line);
                                        same = have && fmt_same(address_of line);
                                }
                        }
                }

                if (fmt_failed)
                        break;

                fmt_words[fmt_word_count - 1].period = true;
                fmt_words[fmt_word_count - 1].final = true;
                fmt_choose_breaks();
                fmt_put_paragraph();
        }
}

static const file_long fmt_longs[] = {
    {(string_address) "crown-margin", 'c'},
    {(string_address) "goal", 'g'},
    {(string_address) "prefix", 'p'},
    {(string_address) "split-only", 's'},
    {(string_address) "tagged-paragraph", 't'},
    {(string_address) "uniform-spacing", 'u'},
    {(string_address) "width", 'w'},
    {null, 0},
};

static b32 text_fmt()
{
        file_taking taking = {
            .program = (string_address) "fmt",
            .allowed = (string_address) "cgpstuw",
            .valued = (string_address) "gpw",
            .longs = fmt_longs,
            .operand = text_file_add,
            .digits = 'W',
        };

        text_begin("fmt");
        text_arena_used = 0;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        if (taking.flags & FILE_FLAG('W'))
        {
                string_address first = program_argument_count() > 1
                                           ? program_argument(1)
                                           : null;

                if (!first || first[0] != '-' || !byte_is_digit(first[1]))
                        return text_refuse(null,
                                           "-WIDTH is only accepted first", 1);
        }

        positive width = FMT_WIDTH_DEFAULT;
        string_address width_value = (taking.flags & FILE_FLAG('w'))
                                         ? file_option_value(address_of taking, 'w')
                                         : (taking.flags & FILE_FLAG('W'))
                                               ? file_option_value(address_of taking, 'W')
                                               : null;

        if (width_value &&
            (!text_unsigned_option(width_value, false, address_of width) ||
             width > 2500))
                return text_refuse(width_value, "invalid width", 1);

        positive goal;

        if (taking.flags & FILE_FLAG('g'))
        {
                string_address value = file_option_value(address_of taking, 'g');
                positive ceiling = width_value ? width : FMT_WIDTH_DEFAULT;

                if (!text_unsigned_option(value, false, address_of goal) ||
                    goal > ceiling)
                        return text_refuse(value, "invalid width", 1);

                if (!width_value)
                        width = goal + 10;
        }
        else
                goal = width * 187 / 200;

        string_address prefix_value = (taking.flags & FILE_FLAG('p'))
                                          ? file_option_value(address_of taking, 'p')
                                          : (string_address) "";
        positive prefix_total = string_length(prefix_value);
        positive leading = 0;
        positive trailing = prefix_total;

        while (leading < prefix_total && prefix_value[leading] == ' ')
                leading++;

        while (trailing > leading && prefix_value[trailing - 1] == ' ')
                trailing--;

        fmt_prefix = prefix_value + leading;
        fmt_prefix_leading = leading;
        fmt_prefix_length = trailing - leading;
        fmt_prefix_full_length = prefix_total - leading;
        fmt_max_width = width;
        fmt_goal_width = goal;
        fmt_crown = (taking.flags & FILE_FLAG('c')) != 0;
        fmt_tagged = (taking.flags & FILE_FLAG('t')) != 0;
        fmt_split = (taking.flags & FILE_FLAG('s')) != 0;
        fmt_uniform = (taking.flags & FILE_FLAG('u')) != 0;
        fmt_failed = false;

        {
                memory_fill(fmt_word_bytes, 1, sizeof(fmt_word_bytes));
                fmt_word_bytes[' '] = 0;
                fmt_word_bytes['\t'] = 0;
                fmt_word_bytes['\n'] = 0;
                fmt_word_bytes['\v'] = 0;
                fmt_word_bytes['\f'] = 0;
                fmt_word_bytes['\r'] = 0;
        }

        fmt_words = (fmt_word address_to)text_arena_take(
            (FMT_WORD_MAX + 1) * sizeof(fmt_word));

        if (!fmt_words)
                return text_done(1);

        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs && !fmt_failed; i++)
        {
                if (!text_open(text_file_name(i)))
                        continue;

                fmt_file();
                text_close();
        }

        return text_done((text_status || fmt_failed) ? 1 : 0);
}

/*
        Page layout.

        pr is another view of the record stream, not another input system.
        One page of small descriptors points into relation_spill; records are
        still obtained by text_line_next, or by the same text_record_cursor
        used by paste when --merge keeps several files live.  This also makes
        the down-column order a permutation of descriptors rather than a
        second collection of lines.
*/
#define PR_LENGTH_DEFAULT 66
#define PR_WIDTH_DEFAULT 72
#define PR_HEADER_LINES 5
#define PR_FOOTER_LINES 5

typedef struct
{
        positive offset;
        positive length;
        bipolar number;
        bool present;
} pr_record;

static pr_record address_to pr_records;
static positive pr_record_room;
static positive pr_spill_used;
static positive pr_columns;
static positive pr_body_lines;
static positive pr_page_length;
static positive pr_page_width;
static positive pr_margin;
static positive pr_first_page;
static positive pr_last_page;
static positive pr_separator_length;
static positive pr_number_digits;
static positive pr_number_width;
static positive pr_input_tab_width;
static positive pr_output_tab_width;
static bipolar pr_line_number;
static bipolar pr_start_line_number;
static string_address pr_separator;
static p8 pr_number_separator;
static string_address pr_header;
static string_address pr_date_format;
static p8 pr_input_tab;
static p8 pr_output_tab;
static bool pr_across;
static bool pr_merge;
static bool pr_double;
static bool pr_form_feed;
static bool pr_join;
static bool pr_number;
static bool pr_number_reset;
static bool pr_omit_header;
static bool pr_omit_pagination;
static bool pr_truncate;
static bool pr_expand_input;
static bool pr_tabify_output;
static bool pr_page_option_failed;
static bool pr_pending;
static positive pr_pending_length;
static bool pr_failed;
static positive pr_output_column;
static b64 pr_now;

static bool pr_parse_positive(string_address value, positive address_to into)
{
        return text_unsigned_option(value, false, into) && address_to into;
}

static bool pr_pages(string_address value)
{
        positive first = 0;
        positive last = positive_max;
        positive used = 0;

        if (!value)
                return false;

        while (byte_is_digit(value[used]))
                used++;

        if (!used)
                return false;

        p8 saved = value[used];
        ((p8 address_to)value)[used] = '\0';
        bool okay = pr_parse_positive(value, address_of first);
        ((p8 address_to)value)[used] = saved;

        if (!okay || !first)
                return false;

        if (saved)
        {
                if (saved != ':' || !value[used + 1] ||
                    !pr_parse_positive(value + used + 1, address_of last) ||
                    last < first)
                        return false;
        }

        pr_first_page = first;
        pr_last_page = last;
        return true;
}

static bool pr_option_seen(p8 letter, string_address value)
{
        if (letter != 'P')
                return true;

        if (pr_pages(value))
                return true;

        text_error(value, "invalid page range");
        pr_page_option_failed = true;
        return false;
}

static fn pr_operand_add(b32 which)
{
        string_address value = program_argument(which);

        if (value[0] == '+' && byte_is_digit(value[1]))
        {
                if (!pr_pages(value + 1))
                {
                        text_error(value, "invalid page range");
                        pr_page_option_failed = true;
                }

                return;
        }

        text_file_add(which);
}

static bool pr_tab_option(string_address value, p8 address_to character,
                          positive address_to width)
{
        address_to character = '\t';
        address_to width = 8;

        if (!value)
                return true;

        positive at = 0;

        if (value[at] && !byte_is_digit(value[at]))
                address_to character = value[at++];

        if (!value[at])
                return true;

        return pr_parse_positive(value + at, width);
}

static bool pr_signed(string_address value, bipolar address_to into)
{
        positive used = 0;
        bipolar made = string_bipolar(value, address_of used);

        if (!value || !used || value[used])
                return false;

        address_to into = made;
        return true;
}

static bool pr_number_option(string_address value)
{
        pr_number_digits = 5;
        pr_number_separator = '\t';

        if (!value)
                return true;

        positive at = 0;

        if (value[at] && !byte_is_digit(value[at]))
                pr_number_separator = value[at++];

        if (value[at] && !pr_parse_positive(value + at,
                                            address_of pr_number_digits))
                return false;

        return pr_number_digits != 0;
}

static fn pr_pad(positive target)
{
        if (target <= pr_output_column)
                return;

        if (pr_tabify_output && pr_output_tab_width)
        {
                for (;;)
                {
                        positive stop =
                            (pr_output_column / pr_output_tab_width + 1) *
                            pr_output_tab_width;

                        if (stop > target || target - pr_output_column < 2)
                                break;

                        text_put_character(pr_output_tab);
                        pr_output_column = stop;
                }
        }

        text_tab_repeat_character(' ', target - pr_output_column);
        pr_output_column = target;
}

static fn pr_put_margin()
{
        pr_output_column = 0;
        text_tab_repeat_character(' ', pr_margin);
        pr_output_column = pr_margin;
}

static fn pr_put_number(bipolar number, positive field_start)
{
        p8 digits[64];
        positive length = bipolar_into_string(digits, number);
        positive blanks = length < pr_number_digits
                              ? pr_number_digits - length
                              : 0;

        if (pr_columns > 1)
                pr_pad(pr_output_column + blanks);
        else
                text_tab_repeat_character(' ', blanks);
        text_put(digits, length);
        pr_output_column += length + (pr_columns > 1 ? 0 : blanks);

        if (pr_columns > 1 && pr_number_separator == '\t')
                /* Multi-column pr treats the default number tab as the
                   remaining field width, then lets ordinary output
                   tabification encode those blanks. */
                pr_pad(field_start + pr_number_width);
        else
        {
                text_put_character(pr_number_separator);

                if (pr_number_separator == '\t')
                        pr_output_column =
                            (pr_output_column / 8 + 1) * 8;
                else
                        pr_output_column++;
        }
}

static fn pr_put_record(pr_record address_to record, positive width)
{
        if (!record->present)
                return;

        p8 address_to bytes = relation_spill + record->offset;
        positive record_column = 0;

        for (positive at = 0; at < record->length; at++)
        {
                p8 character = bytes[at];

                if (pr_expand_input && character == pr_input_tab)
                {
                        positive stop =
                            (record_column / pr_input_tab_width + 1) *
                            pr_input_tab_width;
                        positive count = stop - record_column;

                        if (pr_truncate && stop > width)
                                count = record_column < width
                                            ? width - record_column
                                            : 0;

                        pr_pad(pr_output_column + count);
                        record_column += count;
                        continue;
                }

                if (character == '\b')
                {
                        if (!pr_truncate || record_column)
                        {
                                text_put_character(character);
                                record_column = record_column
                                                    ? record_column - 1
                                                    : 0;
                                pr_output_column = pr_output_column
                                                       ? pr_output_column - 1
                                                       : 0;
                        }
                        continue;
                }

                if (character == '\r')
                {
                        text_put_character(character);
                        record_column = 0;
                        pr_output_column = 0;
                        continue;
                }

                positive after = character == '\t'
                                     ? (record_column / 8 + 1) * 8
                                     : record_column + 1;

                if (pr_truncate && after > width)
                        continue;

                text_put_character(character);
                record_column = after;

                if (character == '\t')
                        pr_output_column =
                            (pr_output_column / 8 + 1) * 8;
                else
                        pr_output_column++;
        }
}

static bool pr_store(p8 address_to bytes, positive length, bipolar number,
                     pr_record address_to record)
{
        positive kept = 0;

        if (length > TEXT_LINE_MAX - pr_spill_used)
        {
                text_error(null, "page is too large");
                pr_failed = true;
                return false;
        }

        record->offset = pr_spill_used;
        record->number = number;
        record->present = true;

        for (positive at = 0; at < length; at++)
                relation_spill[pr_spill_used + kept++] = bytes[at];

        record->length = kept;
        pr_spill_used += kept;
        return true;
}

/* 0: EOF, 1: record, 2: page break, 3: record then page break. */
static b32 pr_source_record(pr_record address_to record)
{
        p8 address_to bytes;
        positive length;

        if (pr_pending)
        {
                bytes = text_record_hold;
                length = pr_pending_length;
                pr_pending = false;
        }
        else
        {
                if (!text_line_next())
                        return 0;

                memory_copy_apart(text_record_hold, text_line,
                                  text_line_length);
                bytes = text_record_hold;
                length = text_line_length;
        }

        /* A form feed terminates the current logical page in every paging
           mode.  With -T the header/trailer goes away, but the separator
           remains observable between the two records. */
        {
                p8 address_to page = memory_first_of(bytes, '\f', length);

                if (page)
                {
                        positive prefix = (positive)(page - bytes);
                        positive suffix = length - prefix - 1;

                        if (!prefix)
                        {
                                if (suffix)
                                {
                                        memory_copy_apart(text_record_hold,
                                                          page + 1, suffix);
                                        pr_pending_length = suffix;
                                        pr_pending = true;
                                }

                                return 2;
                        }

                        if (!pr_store(bytes, prefix, pr_line_number++, record))
                                return 0;

                        if (suffix)
                        {
                                memory_copy_apart(text_record_hold, page + 1,
                                                  suffix);
                                pr_pending_length = suffix;
                                pr_pending = true;
                        }

                        return 3;
                }
        }

        if (!pr_store(bytes, length, pr_line_number++, record))
                return 0;

        return 1;
}

static positive pr_load_page(bool address_to forced)
{
        positive count = 0;
        pr_spill_used = 0;
        address_to forced = false;

        while (count < pr_record_room)
        {
                b32 answer = pr_source_record(pr_records + count);

                if (!answer)
                        break;

                if (answer == 1)
                        count++;
                else
                {
                        if (answer == 3)
                                count++;
                        address_to forced = true;
                        break;
                }
        }

        return count;
}

static positive pr_load_merge(text_record_cursor address_to cursors,
                              positive inputs)
{
        positive rows = 0;
        pr_spill_used = 0;

        while (rows < pr_body_lines)
        {
                bool any = false;

                for (positive column = 0; column < inputs; column++)
                {
                        pr_record address_to record =
                            pr_records + rows * inputs + column;

                        record->present = false;
                        record->length = 0;
                        record->number = pr_line_number;

                        if (cursors[column].reader.failed ||
                            cursors[column].reader.finished)
                                continue;

                        if (text_record_next(cursors + column, '\n', null, 0,
                                             null))
                        {
                                if (memory_first_of(cursors[column].record,
                                                    '\f',
                                                    cursors[column].length))
                                {
                                        text_error(null,
                                                   "form feed with --merge is unsupported");
                                        pr_failed = true;
                                        return rows;
                                }

                                any = true;

                                if (!pr_store(cursors[column].record,
                                              cursors[column].length,
                                              pr_line_number, record))
                                        return rows;
                        }
                        else if (cursors[column].reader.failed)
                                text_status = 1;
                }

                if (!any)
                        break;

                rows++;
                pr_line_number++;
        }

        return rows;
}

static bool pr_date(p8 address_to into, positive room, b64 stamp,
                    positive address_to length)
{
        time_t time = (time_t)stamp;
        tm broken;

        if (!gmtime_r(address_of time, address_of broken))
                return false;

        address_to length = clock_format_extended(into, room, pr_date_format,
                                                   address_of broken);
        return address_to length || !pr_date_format[0];
}

static fn pr_put_header(string_address name, b64 stamp, positive page)
{
        p8 date[512];
        p8 page_text[80];
        positive date_length = 0;
        positive page_digits = positive_into(page_text + 5, page);

        memory_copy(page_text, "Page ", 5);

        if (!pr_date(date, sizeof(date), stamp, address_of date_length))
        {
                text_error(pr_date_format, "date format is too long");
                pr_failed = true;
                return;
        }

        positive name_length = string_length(name);
        positive page_length = page_digits + 5;
        positive occupied = date_length + name_length + page_length;
        positive available = occupied < pr_page_width
                                 ? pr_page_width - occupied
                                 : 0;
        positive left = available / 2;
        positive right = available - left;

        if (!left)
                left = 1;
        if (!right)
                right = 1;

        pr_put_margin();
        text_put_string("\n\n");
        pr_put_margin();
        text_put(date, date_length);
        text_tab_repeat_character(' ', left);
        text_put_string(name);
        text_tab_repeat_character(' ', right);
        text_put(page_text, page_length);
        text_put_string("\n\n\n");
}

static fn pr_put_separator()
{
        text_put(pr_separator, pr_separator_length);
        pr_output_column += pr_separator_length;
}

/* Vertical pages fill each column top-to-bottom.  On an incomplete page GNU
   gives one extra record to each leftmost column, so later column starts are
   cumulative rather than a fixed ceil(count / columns) stride. */
static positive pr_page_index(positive count, positive row, positive column,
                              bool address_to present)
{
        positive index;
        if (pr_merge || pr_across)
                index = row * pr_columns + column;
        else
        {
                positive short_rows = count / pr_columns;
                positive long_columns = count % pr_columns;
                positive column_rows = short_rows +
                                       (column < long_columns);
                if (row >= column_rows)
                {
                        address_to present = false;
                        return count;
                }
                index = column * short_rows +
                        min(column, long_columns) + row;
        }
        address_to present = index < count;
        return index;
}

static fn pr_put_page(positive count, positive rows, positive page,
                      string_address name, b64 stamp, bool forced)
{
        bool shown_header = !pr_omit_header;

        if (shown_header)
                pr_put_header(name, stamp, page);

        positive number_fields = pr_number && pr_merge ? 1 : 0;
        positive fixed = pr_margin +
                         (pr_columns - 1) * pr_separator_length +
                         number_fields * pr_number_width;
        positive useful = pr_page_width > fixed
                              ? pr_page_width - fixed
                              : 0;
        positive column_width = useful / pr_columns;
        positive record_width = pr_number && !pr_merge
                                    ? column_width - pr_number_width
                                    : column_width;

        for (positive row = 0; row < rows; row++)
        {
                pr_put_margin();

                for (positive column = 0; column < pr_columns; column++)
                {
                        bool present;
                        positive index = pr_page_index(count, row, column,
                                                       address_of present);
                        if (!present)
                                break;

                        pr_record address_to record = pr_records + index;
                        positive field_start = pr_output_column;

                        if (pr_number && (!pr_merge || !column))
                                pr_put_number(record->number, field_start);

                        positive data_start = pr_output_column;
                        pr_put_record(record, record_width);

                        bool later = false;
                        if (column + 1 < pr_columns)
                        {
                                bool next_present;
                                (void)pr_page_index(count, row, column + 1,
                                                    address_of next_present);
                                later = next_present;
                        }

                        if (later)
                        {
                                bool blank_separator =
                                    pr_separator_length == 1 &&
                                    pr_separator[0] == ' ';

                                if (!pr_join)
                                        pr_pad(data_start + record_width +
                                               blank_separator);

                                if (!blank_separator)
                                        pr_put_separator();
                        }
                }

                text_put_character('\n');

                if (pr_double)
                        text_put_character('\n');
        }

        if (shown_header)
        {
                if (pr_form_feed)
                        text_put_character('\f');
                else
                {
                        positive used = PR_HEADER_LINES +
                                        rows * (pr_double ? 2 : 1);

                        if (used < pr_page_length)
                                text_tab_repeat_character('\n',
                                                          pr_page_length - used);
                }
        }
        else if (forced)
                text_put_character('\f');
}

static b64 pr_stamp(positive handle)
{
        file_facts facts;

        if (text_handle_facts(handle, address_of facts))
                return (b64)facts.modified.seconds;

        return pr_now;
}

static fn pr_single_file(string_address path)
{
        if (!text_open(path))
                return;

        b64 stamp = pr_stamp(text_input.handle);
        string_address heading = pr_header ? pr_header
                                           : (path ? path
                                                   : (string_address)"");
        positive page = 1;

        pr_pending = false;
        pr_pending_length = 0;
        pr_line_number = pr_number_reset ? pr_start_line_number : 1;

        while (!pr_failed)
        {
                if (pr_number_reset && page == pr_first_page)
                        pr_line_number = pr_start_line_number;

                bool forced;
                positive count = pr_load_page(address_of forced);

                if (!count && !forced)
                        break;

                positive rows = pr_columns == 1
                                    ? count
                                    : (count + pr_columns - 1) / pr_columns;

                if (page >= pr_first_page && page <= pr_last_page)
                        pr_put_page(count, rows, page, heading, stamp, forced);

                if (page >= pr_last_page)
                        break;

                page++;
        }

        text_close();
}

static fn pr_merge_files()
{
        positive inputs = text_files_count;
        text_record_cursor address_to cursors =
            (text_record_cursor address_to)text_arena_take(
                inputs * sizeof(text_record_cursor));

        if (!cursors)
        {
                pr_failed = true;
                return;
        }

        for (positive input = 0; input < inputs; input++)
        {
                string_address path = text_file_name(input);

                if (!text_record_open(cursors + input, path,
                                      text_record_hold))
                        text_status = 1;
        }

        positive page = 1;
        pr_line_number = pr_number_reset ? pr_start_line_number : 1;

        while (!pr_failed)
        {
                if (pr_number_reset && page == pr_first_page)
                        pr_line_number = pr_start_line_number;

                positive rows = pr_load_merge(cursors, inputs);

                if (!rows)
                        break;

                if (page >= pr_first_page && page <= pr_last_page)
                        pr_put_page(rows * inputs, rows, page,
                                    pr_header ? pr_header
                                              : (string_address)"",
                                    pr_now, false);

                if (page >= pr_last_page)
                        break;

                page++;
        }

        for (positive input = 0; input < inputs; input++)
                text_record_close(cursors + input);
}

static const file_long pr_longs[] = {
    {(string_address)"pages", 'P'},
    {(string_address)"columns", 'C'},
    {(string_address)"across", 'a'},
    {(string_address)"show-control-chars", 'c'},
    {(string_address)"double-space", 'd'},
    {(string_address)"date-format", 'D'},
    {(string_address)"expand-tabs", 'e'},
    {(string_address)"form-feed", 'F'},
    {(string_address)"header", 'h'},
    {(string_address)"output-tabs", 'i'},
    {(string_address)"join-lines", 'J'},
    {(string_address)"length", 'l'},
    {(string_address)"merge", 'm'},
    {(string_address)"number-lines", 'n'},
    {(string_address)"first-line-number", 'N'},
    {(string_address)"indent", 'o'},
    {(string_address)"no-file-warnings", 'r'},
    {(string_address)"separator", 's'},
    {(string_address)"sep-string", 'S'},
    {(string_address)"omit-header", 't'},
    {(string_address)"omit-pagination", 'T'},
    {(string_address)"show-nonprinting", 'v'},
    {(string_address)"width", 'w'},
    {(string_address)"page-width", 'W'},
    {null, 0},
};

static b32 text_pr()
{
        file_taking taking = {
            .program = (string_address)"pr",
            .allowed = (string_address)"acDdeFfhJilmnNorsStTvwW",
            .valued = (string_address)"PCDhlNoWw",
            .optional = (string_address)"einsS",
            .sticky_optional = (string_address)"einsS",
            .longs = pr_longs,
            .operand = pr_operand_add,
            .seen = pr_option_seen,
            .digits = 'C',
        };

        text_begin("pr");
        text_arena_used = 0;
        pr_page_option_failed = false;
        pr_first_page = 1;
        pr_last_page = positive_max;

        if (!file_take(address_of taking) || pr_page_option_failed ||
            !text_files_ready())
                return text_done(1);

        if ((taking.flags & FILE_FLAG('c')) ||
            (taking.flags & FILE_FLAG('v')))
                return text_refuse(null,
                                   "control-character display is unsupported",
                                   1);

        pr_merge = (taking.flags & FILE_FLAG('m')) != 0;
        pr_across = (taking.flags & FILE_FLAG('a')) != 0;

        if (pr_merge && (pr_across || (taking.flags & FILE_FLAG('C'))))
                return text_refuse(null,
                                   "cannot combine --merge and --columns",
                                   1);

        if (pr_merge && !text_files_count)
                pr_merge = false;

        pr_columns = pr_merge ? text_files_count : 1;

        if (!pr_merge && (taking.flags & FILE_FLAG('C')) &&
            (!pr_parse_positive(file_option_value(address_of taking, 'C'),
                                address_of pr_columns) ||
             pr_columns > TEXT_LINE_MAX))
                return text_refuse(file_option_value(address_of taking, 'C'),
                                   "invalid number of columns", 1);

        pr_page_length = PR_LENGTH_DEFAULT;
        pr_page_width = PR_WIDTH_DEFAULT;
        pr_margin = 0;

        if ((taking.flags & FILE_FLAG('l')) &&
            !pr_parse_positive(file_option_value(address_of taking, 'l'),
                               address_of pr_page_length))
                return text_refuse(file_option_value(address_of taking, 'l'),
                                   "invalid page length", 1);

        p8 width_letter = (taking.flags & FILE_FLAG('W')) ? 'W' : 'w';

        if ((taking.flags & (FILE_FLAG('W') | FILE_FLAG('w'))) &&
            !pr_parse_positive(file_option_value(address_of taking,
                                                  width_letter),
                               address_of pr_page_width))
                return text_refuse(file_option_value(address_of taking,
                                                      width_letter),
                                   "invalid page width", 1);

        if ((taking.flags & FILE_FLAG('o')) &&
            !text_unsigned_option(file_option_value(address_of taking, 'o'),
                                  false, address_of pr_margin))
                return text_refuse(file_option_value(address_of taking, 'o'),
                                   "invalid indentation", 1);

        pr_omit_pagination = (taking.flags & FILE_FLAG('T')) != 0;
        pr_omit_header = pr_omit_pagination ||
                         (taking.flags & FILE_FLAG('t')) ||
                         pr_page_length <= PR_HEADER_LINES + PR_FOOTER_LINES;
        pr_double = (taking.flags & FILE_FLAG('d')) != 0;
        pr_form_feed = (taking.flags & (FILE_FLAG('F') | FILE_FLAG('f'))) != 0;
        pr_join = (taking.flags & FILE_FLAG('J')) != 0;
        pr_header = (taking.flags & FILE_FLAG('h'))
                        ? file_option_value(address_of taking, 'h')
                        : null;
        pr_date_format = (taking.flags & FILE_FLAG('D'))
                             ? file_option_value(address_of taking, 'D')
                             : (string_address)"%Y-%m-%d %H:%M";

        if (pr_header && pr_omit_header)
                return text_refuse(null,
                                   "header conflicts with omitted pagination",
                                   1);

        positive printable = pr_omit_header
                                 ? pr_page_length
                                 : pr_page_length - PR_HEADER_LINES -
                                       PR_FOOTER_LINES;
        pr_body_lines = pr_double ? (printable > 1 ? printable / 2 : 1)
                                  : printable;

        if (!pr_body_lines)
                return text_refuse(null, "page length leaves no body", 1);

        pr_separator = pr_join ? (string_address)"\t"
                               : (string_address)" ";

        if (taking.flags & FILE_FLAG('S'))
                pr_separator = file_option_value(address_of taking, 'S')
                                   ? file_option_value(address_of taking, 'S')
                                   : (string_address)"";
        else if (taking.flags & FILE_FLAG('s'))
                pr_separator = file_option_value(address_of taking, 's')
                                   ? file_option_value(address_of taking, 's')
                                   : (string_address)"\t";

        pr_separator_length = string_length(pr_separator);
        pr_number = (taking.flags & FILE_FLAG('n')) != 0;
        pr_start_line_number = 1;
        pr_number_reset = (taking.flags & FILE_FLAG('N')) != 0;

        if (pr_number &&
            !pr_number_option(file_option_value(address_of taking, 'n')))
                return text_refuse(file_option_value(address_of taking, 'n'),
                                   "invalid line-number format", 1);

        if (pr_number_reset &&
            !pr_signed(file_option_value(address_of taking, 'N'),
                       address_of pr_start_line_number))
                return text_refuse(file_option_value(address_of taking, 'N'),
                                   "invalid first line number", 1);

        pr_number_width = pr_number
                              ? (pr_number_separator == '\t'
                                     ? ((pr_number_digits / 8) + 1) * 8
                                     : pr_number_digits + 1)
                              : 0;

        pr_input_tab = '\t';
        pr_input_tab_width = 8;
        pr_output_tab = '\t';
        pr_output_tab_width = 8;

        if ((taking.flags & FILE_FLAG('e')) &&
            !pr_tab_option(file_option_value(address_of taking, 'e'),
                           address_of pr_input_tab,
                           address_of pr_input_tab_width))
                return text_refuse(file_option_value(address_of taking, 'e'),
                                   "invalid tab width", 1);

        if ((taking.flags & FILE_FLAG('i')) &&
            !pr_tab_option(file_option_value(address_of taking, 'i'),
                           address_of pr_output_tab,
                           address_of pr_output_tab_width))
                return text_refuse(file_option_value(address_of taking, 'i'),
                                   "invalid tab width", 1);

        pr_expand_input = (taking.flags & FILE_FLAG('e')) || pr_columns > 1;
        pr_tabify_output = (taking.flags & FILE_FLAG('i')) || pr_columns > 1;
        /* GNU turns truncation on for a page that really has more than one
           column, lets a lone -s take it back off, and lets an explicit width
           or -W put it back; -J is the only switch that overrides all three.
           A bare -1, or -m over a single file, is still one column and leaves
           long records whole. */
        bool given_width = (taking.flags & FILE_FLAG('w')) != 0;

        pr_truncate = !pr_join &&
                      ((taking.flags & FILE_FLAG('W')) != 0 ||
                       (given_width &&
                        (pr_merge ||
                         (taking.flags & FILE_FLAG('C')) != 0)) ||
                       (!given_width && pr_columns > 1 &&
                        !(taking.flags & FILE_FLAG('s'))));
        text_quiet_open = (taking.flags & FILE_FLAG('r')) != 0;

        positive number_fields = pr_number && pr_merge ? 1 : 0;
        positive fixed = pr_margin +
                         (pr_columns - 1) * pr_separator_length +
                         number_fields * pr_number_width;

        if (fixed >= pr_page_width ||
            (pr_page_width - fixed) / pr_columns <=
                (pr_number && !pr_merge ? pr_number_width : 0))
                return text_refuse(null, "page width is too narrow", 1);

        if (pr_body_lines > positive_max / pr_columns ||
            pr_body_lines * pr_columns > TEXT_LINE_MAX)
                return text_refuse(null, "page has too many records", 1);

        pr_record_room = pr_body_lines * pr_columns;
        pr_records = (pr_record address_to)text_arena_take(
            pr_record_room * sizeof(pr_record));

        if (!pr_records)
                return text_done(1);

        pr_now = file_now();
        pr_failed = false;

        if (pr_merge)
                pr_merge_files();
        else
        {
                b32 inputs = text_input_count();

                for (b32 input = 0; input < inputs && !pr_failed; input++)
                        pr_single_file(text_file_name(input));
        }

        return text_done((text_status || pr_failed) ? 1 : 0);
}

/*
        Permuted index.

        Source bytes live in text_arena and arrive through text_reader.  The
        occurrence table contains only offsets into those bytes, and its
        order is produced by the same stable merge sorter and byte comparator
        as sort.  ptx therefore adds a context planner, not another reader,
        tokenizer, sorting engine or output buffer.
*/
typedef struct
{
        p8 address_to bytes;
        positive length;
} text_blob;

typedef struct
{
        text_blob text;
        string_address name;
        positive lines;
} ptx_file;

typedef struct
{
        positive file;
        positive start;
        positive finish;
        positive content;
        positive reference;
        positive reference_length;
        positive line;
} ptx_context;

typedef struct
{
        positive file;
        positive key;
        positive key_length;
        positive left;
        positive right;
        positive reference;
        positive reference_length;
        positive line;
        positive order;
} ptx_occurrence;

typedef struct
{
        positive start;
        positive finish;
        bool have;
} ptx_span;

static ptx_file address_to ptx_files;
static positive ptx_file_count;
static ptx_context address_to ptx_contexts;
static positive ptx_context_count;
static ptx_occurrence address_to ptx_occurrences;
static positive ptx_occurrence_count;
static positive address_to ptx_order;
static text_blob ptx_ignore;
static text_blob ptx_only;
static string_address ptx_sentence_pattern;
static string_address ptx_word_pattern;
static string_address ptx_truncation;
static positive ptx_truncation_length;
static positive ptx_width;
static positive ptx_gap;
static positive ptx_reference_width;
static positive ptx_half_width;
static positive ptx_before_width;
static positive ptx_keyafter_width;
static positive ptx_maximum_word;
static bool ptx_fold;
static bool ptx_auto_reference;
static bool ptx_input_reference;
static bool ptx_right_reference;
static bool ptx_custom_sentence;
static bool ptx_custom_word;
static bool ptx_lower_word;
static bool ptx_alpha_word;
static bool ptx_failed;

/* A whole-input view in the shared record arena.  ptx and column both need
   stable offsets after the 64 KiB reader refills; this is their one common
   bridge from the streaming reader, not a second input engine. */
static bool text_blob_read(string_address path, text_blob address_to blob)
{
        bool failed = false;

        if (path && !path[0])
                path = null;

        if (!text_open(path))
                return false;

        if (!text_arena_take(0))
        {
                text_close();
                return false;
        }

        blob->bytes = text_arena + text_arena_used;
        blob->length = 0;

        while (text_fill())
        {
                positive left = text_input.filled - text_input.position;

                if (text_arena_used > TEXT_ARENA_BYTES ||
                    left > TEXT_ARENA_BYTES - text_arena_used)
                {
                        text_error(null, "input too large");
                        text_status = text_status ? text_status : 1;
                        failed = true;
                        break;
                }

                memory_copy_apart(text_arena + text_arena_used,
                                  text_input.buffer + text_input.position,
                                  left);
                text_arena_used += left;
                blob->length += left;
                text_input.position = text_input.filled;
        }

        text_close();

        if (text_arena_used > positive_max - 15 ||
            ((text_arena_used + 15) & ~(positive)15) > TEXT_ARENA_BYTES)
        {
                text_error(null, "input too large");
                text_status = text_status ? text_status : 1;
                return false;
        }

        text_arena_used = (text_arena_used + 15) & ~(positive)15;
        return !failed;
}

static positive ptx_count_lines(text_blob address_to text)
{
        positive lines = text->length ? 1 : 0;

        for (positive at = 0; at < text->length; at++)
                if (text->bytes[at] == '\n')
                        lines++;

        return lines;
}

static positive ptx_skip_white(ptx_file address_to file, positive at,
                               positive limit)
{
        while (at < limit && byte_is_space(file->text.bytes[at]))
                at++;

        return at;
}

static positive ptx_trim_white(ptx_file address_to file, positive at,
                               positive floor)
{
        while (at > floor && byte_is_space(file->text.bytes[at - 1]))
                at--;

        return at;
}

static positive ptx_default_sentence(ptx_file address_to file, positive from,
                                     positive address_to match)
{
        p8 address_to bytes = file->text.bytes;
        positive length = file->text.length;

        for (positive at = from; at < length; at++)
        {
                if (bytes[at] != '.' && bytes[at] != '?' && bytes[at] != '!')
                        continue;

                positive after = at + 1;

                while (after < length &&
                       (bytes[after] == ']' || bytes[after] == '"' ||
                        bytes[after] == '\'' || bytes[after] == ')' ||
                        bytes[after] == '}'))
                        after++;

                bool boundary = after == length || bytes[after] == '\n' ||
                                bytes[after] == '\t' ||
                                (bytes[after] == ' ' && after + 1 < length &&
                                 bytes[after + 1] == ' ');

                if (!boundary)
                        continue;

                address_to match = at;

                while (after < length &&
                       (bytes[after] == ' ' || bytes[after] == '\t' ||
                        bytes[after] == '\n'))
                        after++;

                return after;
        }

        address_to match = length;
        return length;
}

static positive ptx_context_next(ptx_file address_to file, positive from,
                                 positive address_to visible)
{
        positive after;

        if (ptx_input_reference && !ptx_custom_sentence)
        {
                p8 address_to newline = memory_first_of(
                    file->text.bytes + from, '\n', file->text.length - from);
                after = newline ? (positive)(newline - file->text.bytes) + 1
                                : file->text.length;
        }
        else if (!ptx_custom_sentence)
        {
                positive match;
                after = ptx_default_sentence(file, from, address_of match);

                if (match == from)
                {
                        text_error(null,
                                   "sentence expression matches empty text");
                        ptx_failed = true;
                        return file->text.length;
                }
        }
        else if (!ptx_sentence_pattern[0])
                after = file->text.length;
        else if (regex_search_longest(file->text.bytes + from,
                                      file->text.length - from, 0))
        {
                positive begin = from + regex_slots[0];
                after = from + regex_slots[1];

                if (begin == from || after == begin)
                {
                        text_error(ptx_sentence_pattern,
                                   "sentence expression matches empty text");
                        ptx_failed = true;
                        return file->text.length;
                }
        }
        else
                after = file->text.length;

        address_to visible = ptx_trim_white(file, after, from);
        return after;
}

static positive ptx_plan_contexts(bool fill)
{
        positive made = 0;

        for (positive file_index = 0; file_index < ptx_file_count; file_index++)
        {
                ptx_file address_to file = ptx_files + file_index;
                positive cursor = 0;
                positive line = 1;

                while (cursor < file->text.length && !ptx_failed)
                {
                        positive visible;
                        positive after = ptx_context_next(file, cursor,
                                                          address_of visible);

                        if (fill)
                        {
                                ptx_context address_to context =
                                    ptx_contexts + made;
                                context->file = file_index;
                                context->start = cursor;
                                context->finish = visible;
                                context->content = cursor;
                                context->reference = cursor;
                                context->reference_length = 0;
                                context->line = line;

                                if (ptx_input_reference)
                                {
                                        positive scan = cursor;

                                        while (scan < visible &&
                                               !byte_is_space(
                                                   file->text.bytes[scan]))
                                                scan++;

                                        context->reference_length = scan - cursor;
                                        context->content = ptx_skip_white(
                                            file, scan, visible);

                                        if (context->reference_length >
                                            ptx_reference_width)
                                                ptx_reference_width =
                                                    context->reference_length;
                                }
                        }

                        for (positive at = cursor; at < after; at++)
                                if (file->text.bytes[at] == '\n')
                                        line++;

                        made++;

                        if (after <= cursor)
                                break;

                        cursor = after;
                }
        }

        return made;
}

static bool ptx_word_equal(p8 address_to one, positive one_length,
                           p8 address_to two, positive two_length)
{
        return !sort_compare_bytes(one, one_length, two, two_length,
                                   ptx_fold ? 1 : 0);
}

static bool ptx_list_has(text_blob address_to list, p8 address_to word,
                         positive length)
{
        positive at = 0;

        while (at < list->length)
        {
                positive from = at;

                while (at < list->length && list->bytes[at] != '\n')
                        at++;

                if (at > from &&
                    ptx_word_equal(word, length, list->bytes + from,
                                   at - from))
                        return true;

                if (at < list->length)
                        at++;
        }

        return false;
}

static bool ptx_selected(p8 address_to word, positive length)
{
        if (ptx_ignore.length && ptx_list_has(address_of ptx_ignore,
                                              word, length))
                return false;

        return !ptx_only.length ||
               ptx_list_has(address_of ptx_only, word, length);
}

static bool ptx_next_word(ptx_context address_to context,
                          positive address_to cursor,
                          positive address_to start,
                          positive address_to finish)
{
        ptx_file address_to file = ptx_files + context->file;
        p8 address_to bytes = file->text.bytes;
        positive at = address_to cursor;

        if (ptx_custom_word)
        {
                if (!regex_search_longest(bytes + at,
                                          context->finish - at, 0))
                        return false;

                address_to start = at + regex_slots[0];
                address_to finish = at + regex_slots[1];
                address_to cursor = address_to finish > at
                                        ? address_to finish
                                        : at + 1;
                return true;
        }

        while (at < context->finish && !fmt_word_bytes[bytes[at]])
                at++;

        if (at == context->finish)
                return false;

        address_to start = at;

        while (at < context->finish && fmt_word_bytes[bytes[at]])
                at++;

        address_to finish = at;
        address_to cursor = at;
        return true;
}

static positive ptx_word_line(ptx_context address_to context, positive word)
{
        ptx_file address_to file = ptx_files + context->file;
        positive line = context->line;

        for (positive at = context->start; at < word; at++)
                if (file->text.bytes[at] == '\n')
                        line++;

        return line;
}

static positive ptx_scan_occurrences(bool fill)
{
        positive made = 0;

        for (positive context_index = 0;
             context_index < ptx_context_count; context_index++)
        {
                ptx_context address_to context = ptx_contexts + context_index;
                ptx_file address_to file = ptx_files + context->file;
                positive cursor = context->content;
                positive start;
                positive finish;

                while (cursor < context->finish &&
                       ptx_next_word(context, address_of cursor,
                                     address_of start, address_of finish))
                {
                        positive length = finish - start;

                        if (!length)
                                continue;

                        if (length > ptx_maximum_word)
                                ptx_maximum_word = length;

                        if (!ptx_selected(file->text.bytes + start, length))
                                continue;

                        if (fill)
                        {
                                ptx_occurrence address_to occurrence =
                                    ptx_occurrences + made;
                                occurrence->file = context->file;
                                occurrence->key = start;
                                occurrence->key_length = length;
                                occurrence->left = context->content;
                                occurrence->right = context->finish;
                                occurrence->reference = context->reference;
                                occurrence->reference_length =
                                    context->reference_length;
                                occurrence->line = ptx_word_line(context,
                                                                 start);
                                occurrence->order = made;
                        }

                        made++;
                }
        }

        return made;
}

static PURE HOT bipolar ptx_compare(positive left, positive right)
{
        ptx_occurrence address_to one = ptx_occurrences + left;
        ptx_occurrence address_to two = ptx_occurrences + right;
        ptx_file address_to one_file = ptx_files + one->file;
        ptx_file address_to two_file = ptx_files + two->file;
        bipolar answer = sort_compare_bytes(
            one_file->text.bytes + one->key, one->key_length,
            two_file->text.bytes + two->key, two->key_length,
            ptx_fold ? 1 : 0);

        if (answer)
                return answer;

        return one->order < two->order ? -1 : one->order != two->order;
}

static positive ptx_skip_something(ptx_file address_to file, positive at,
                                   positive limit)
{
        if (at >= limit)
                return at;

        if (ptx_custom_word)
        {
                if (regex_match_longest(file->text.bytes + at,
                                        limit - at, 0) && regex_slots[1])
                        return at + regex_slots[1];

                return at + 1;
        }

        if (fmt_word_bytes[file->text.bytes[at]])
                while (at < limit && fmt_word_bytes[file->text.bytes[at]])
                        at++;
        else
                at++;

        return at;
}

static fn ptx_put_spaces(positive count)
{
        text_tab_repeat_character(' ', count);
}

static positive ptx_field_padding(positive field, bipolar used)
{
        if (used < 0)
        {
                positive extra = (positive)(-used);
                return extra <= positive_max - field ? field + extra
                                                     : positive_max;
        }

        return (positive)used < field ? field - (positive)used : 0;
}

static fn ptx_put_span(ptx_file address_to file, ptx_span span)
{
        if (!span.have)
                return;

        positive from = span.start;

        while (from < span.finish)
        {
                positive at = from;

                while (at < span.finish &&
                       !byte_is_space(file->text.bytes[at]))
                        at++;

                text_put(file->text.bytes + from, at - from);

                if (at < span.finish)
                {
                        text_put_character(' ');
                        at++;
                }

                from = at;
        }
}

static positive ptx_decimal_length(positive number)
{
        p8 digits[64];
        return positive_into(digits, number);
}

static positive ptx_reference_length(ptx_occurrence address_to occurrence)
{
        if (ptx_auto_reference)
        {
                string_address name = ptx_files[occurrence->file].name;
                return (name ? string_length(name) : 0) + 1 +
                       ptx_decimal_length(occurrence->line);
        }

        return occurrence->reference_length;
}

static fn ptx_put_reference(ptx_occurrence address_to occurrence)
{
        ptx_file address_to file = ptx_files + occurrence->file;

        if (ptx_auto_reference)
        {
                if (file->name)
                        text_put_string(file->name);

                text_put_character(':');
                p8 digits[64];
                positive length = positive_into(digits, occurrence->line);
                text_put(digits, length);
        }
        else if (ptx_input_reference)
                ptx_put_span(file, (ptx_span){occurrence->reference,
                                              occurrence->reference +
                                                  occurrence->reference_length,
                                              true});
}

static fn ptx_output_one(ptx_occurrence address_to occurrence)
{
        ptx_file address_to file = ptx_files + occurrence->file;
        positive key_start = occurrence->key;
        positive key_finish = key_start + occurrence->key_length;
        positive left_context = occurrence->left;
        positive right_context = occurrence->right;
        positive keyafter_finish = key_finish;
        positive cursor = key_finish;

        while (cursor < right_context &&
               cursor <= key_start + ptx_keyafter_width)
        {
                keyafter_finish = cursor;
                cursor = ptx_skip_something(file, cursor, right_context);
        }

        if (cursor <= key_start + ptx_keyafter_width)
                keyafter_finish = cursor;

        bool keyafter_truncated = ptx_truncation_length &&
                                  keyafter_finish < right_context;
        keyafter_finish = ptx_trim_white(file, keyafter_finish, key_start);

        positive left_field_start;

        if (key_start - left_context > ptx_half_width + ptx_maximum_word)
        {
                left_field_start =
                    key_start - (ptx_half_width + ptx_maximum_word);
                left_field_start = ptx_skip_something(file,
                                                       left_field_start,
                                                       key_start);
        }
        else
                left_field_start = left_context;
        positive before_start = left_field_start;
        positive before_finish = ptx_trim_white(file, key_start, before_start);

        while (before_start < before_finish &&
               before_finish - before_start > ptx_before_width)
                before_start = ptx_skip_something(file, before_start,
                                                  before_finish);

        positive before_probe = ptx_trim_white(file, before_start, 0);
        bool before_truncated = ptx_truncation_length &&
                                before_probe > left_context;
        before_start = ptx_skip_white(file, before_start, file->text.length);

        ptx_span tail = {0, 0, false};
        bool tail_truncated = false;
        bipolar tail_room = (bipolar)ptx_before_width -
                            (bipolar)(before_finish - before_start) -
                            (bipolar)ptx_gap;

        if (tail_room > 0)
        {
                tail.start = ptx_skip_white(file, keyafter_finish,
                                            file->text.length);
                tail.finish = tail.start;
                cursor = tail.start;

                while (cursor < right_context &&
                       cursor - tail.start < (positive)tail_room)
                {
                        tail.finish = cursor;
                        cursor = ptx_skip_something(file, cursor,
                                                    right_context);
                }

                if (cursor - tail.start < (positive)tail_room)
                        tail.finish = cursor;

                if (tail.finish > tail.start)
                {
                        tail.have = true;
                        keyafter_truncated = false;
                        tail_truncated = ptx_truncation_length &&
                                         tail.finish < right_context;
                        tail.finish = ptx_trim_white(file, tail.finish,
                                                     tail.start);
                }
        }

        ptx_span head = {0, 0, false};
        bool head_truncated = false;
        bipolar head_room = (bipolar)ptx_keyafter_width -
                            (bipolar)(keyafter_finish - key_start) -
                            (bipolar)ptx_gap;

        if (head_room > 0)
        {
                head.finish = ptx_trim_white(file, before_start, 0);
                head.start = left_field_start;

                while (head.start < head.finish &&
                       head.finish - head.start > (positive)head_room)
                        head.start = ptx_skip_something(file, head.start,
                                                       head.finish);

                if (head.finish > head.start)
                {
                        head.have = true;
                        before_truncated = false;
                        head_truncated = ptx_truncation_length &&
                                         head.start > left_context;
                        head.start = ptx_skip_white(file, head.start,
                                                   head.finish);
                }
        }

        positive reference_length = ptx_reference_length(occurrence);

        if (!ptx_right_reference)
        {
                if (ptx_auto_reference)
                {
                        ptx_put_reference(occurrence);
                        text_put_character(':');
                        positive used = reference_length + 1;
                        positive field = ptx_reference_width + ptx_gap;
                        ptx_put_spaces(field > used ? field - used : 0);
                }
                else
                {
                        if (ptx_input_reference)
                                ptx_put_reference(occurrence);

                        positive field = ptx_reference_width + ptx_gap;
                        ptx_put_spaces(field > reference_length
                                           ? field - reference_length
                                           : 0);
                }
        }

        bipolar before_length = (bipolar)before_finish -
                                  (bipolar)before_start;

        if (tail.have)
        {
                ptx_put_span(file, tail);

                if (tail_truncated)
                        text_put(ptx_truncation, ptx_truncation_length);

                bipolar used = before_length +
                               (bipolar)(tail.finish - tail.start) +
                               (before_truncated
                                    ? (bipolar)ptx_truncation_length
                                    : 0) +
                               (tail_truncated
                                    ? (bipolar)ptx_truncation_length
                                    : 0);
                positive field = ptx_half_width > ptx_gap
                                     ? ptx_half_width - ptx_gap
                                     : 0;
                ptx_put_spaces(ptx_field_padding(field, used));
        }
        else
        {
                bipolar used = before_length +
                               (before_truncated
                                    ? (bipolar)ptx_truncation_length
                                    : 0);
                positive field = ptx_half_width > ptx_gap
                                     ? ptx_half_width - ptx_gap
                                     : 0;
                ptx_put_spaces(ptx_field_padding(field, used));
        }

        if (before_truncated)
                text_put(ptx_truncation, ptx_truncation_length);

        ptx_put_span(file, (ptx_span){before_start, before_finish, true});
        ptx_put_spaces(ptx_gap);
        ptx_put_span(file, (ptx_span){key_start, keyafter_finish, true});

        if (keyafter_truncated)
                text_put(ptx_truncation, ptx_truncation_length);

        if (head.have)
        {
                positive used = keyafter_finish - key_start +
                                head.finish - head.start +
                                (keyafter_truncated ? ptx_truncation_length : 0) +
                                (head_truncated ? ptx_truncation_length : 0);
                ptx_put_spaces(ptx_half_width > used
                                   ? ptx_half_width - used
                                   : 0);

                if (head_truncated)
                        text_put(ptx_truncation, ptx_truncation_length);

                ptx_put_span(file, head);
        }
        else if ((ptx_auto_reference || ptx_input_reference) &&
                 ptx_right_reference)
        {
                positive used = keyafter_finish - key_start +
                                (keyafter_truncated ? ptx_truncation_length : 0);
                ptx_put_spaces(ptx_half_width > used
                                   ? ptx_half_width - used
                                   : 0);
        }

        if ((ptx_auto_reference || ptx_input_reference) &&
            ptx_right_reference)
        {
                ptx_put_spaces(ptx_gap);
                ptx_put_reference(occurrence);
        }

        text_put_character('\n');
}

static const file_long ptx_longs[] = {
    {(string_address)"auto-reference", 'A'},
    {(string_address)"traditional", 'G'},
    {(string_address)"flag-truncation", 'F'},
    {(string_address)"macro-name", 'M'},
    {(string_address)"format", 'Q'},
    {(string_address)"right-side-refs", 'R'},
    {(string_address)"sentence-regexp", 'S'},
    {(string_address)"word-regexp", 'W'},
    {(string_address)"break-file", 'b'},
    {(string_address)"ignore-case", 'f'},
    {(string_address)"gap-size", 'g'},
    {(string_address)"ignore-file", 'i'},
    {(string_address)"only-file", 'o'},
    {(string_address)"references", 'r'},
    {(string_address)"typeset-mode", 't'},
    {(string_address)"width", 'w'},
    {null, 0},
};

static positive ptx_unescape(p8 address_to text)
{
        positive from = 0;
        positive into = 0;

        while (text[from])
        {
                if (text[from] != '\\')
                {
                        text[into++] = text[from++];
                        continue;
                }

                from++;
                p8 escaped = text[from];

                if (!escaped)
                        break;

                if (escaped == 'x')
                {
                        positive value = 0;
                        positive digits = 0;
                        from++;

                        while (digits < 3 && byte_is_hexadecimal(text[from]))
                        {
                                p8 digit = text[from++];
                                value = value * 16 +
                                        (digit <= '9'
                                             ? digit - '0'
                                             : (digit | 32) - 'a' + 10);
                                digits++;
                        }

                        if (digits)
                                text[into++] = (p8)value;
                        else
                        {
                                text[into++] = '\\';
                                text[into++] = 'x';
                        }

                        continue;
                }

                if (escaped == '0')
                {
                        positive value = 0;
                        positive digits = 0;
                        from++;

                        while (digits < 3 && text[from] >= '0' &&
                               text[from] <= '7')
                        {
                                value = value * 8 + text[from++] - '0';
                                digits++;
                        }

                        text[into++] = (p8)value;
                        continue;
                }

                from++;

                if (escaped == 'a')
                        text[into++] = '\a';
                else if (escaped == 'b')
                        text[into++] = '\b';
                else if (escaped == 'c')
                {
                        while (text[from])
                                from++;
                }
                else if (escaped == 'f')
                        text[into++] = '\f';
                else if (escaped == 'n')
                        text[into++] = '\n';
                else if (escaped == 'r')
                        text[into++] = '\r';
                else if (escaped == 't')
                        text[into++] = '\t';
                else if (escaped == 'v')
                        text[into++] = '\v';
                else
                {
                        text[into++] = '\\';
                        text[into++] = escaped;
                }
        }

        text[into] = '\0';
        return into;
}

static b32 text_ptx()
{
        file_taking taking = {
            .program = (string_address)"ptx",
            .allowed = (string_address)"AFGMORSTWbfgiortw",
            .valued = (string_address)"FMSWbgiowQ",
            .longs = ptx_longs,
            .operand = text_file_add,
        };

        text_begin("ptx");
        text_arena_used = 0;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        positive flags = taking.flags;

        if (flags & (FILE_FLAG('G') | FILE_FLAG('O') | FILE_FLAG('T') |
                     FILE_FLAG('Q')))
                return text_refuse(null,
                                   "traditional and typesetter formats are unsupported",
                                   1);

        ptx_fold = (flags & FILE_FLAG('f')) != 0;
        ptx_auto_reference = (flags & FILE_FLAG('A')) != 0;
        ptx_input_reference = (flags & FILE_FLAG('r')) != 0;
        ptx_right_reference = (flags & FILE_FLAG('R')) != 0;
        ptx_sentence_pattern = (flags & FILE_FLAG('S'))
                                   ? file_option_value(address_of taking, 'S')
                                   : null;
        ptx_word_pattern = (flags & FILE_FLAG('W'))
                               ? file_option_value(address_of taking, 'W')
                               : null;
        ptx_custom_sentence = ptx_sentence_pattern != null;
        ptx_custom_word = ptx_word_pattern && ptx_word_pattern[0];
        ptx_lower_word = false;
        ptx_alpha_word = false;
        ptx_truncation = (flags & FILE_FLAG('F'))
                             ? file_option_value(address_of taking, 'F')
                             : (string_address)"/";
        ptx_width = (flags & FILE_FLAG('t')) ? 100 : 72;
        ptx_gap = 3;
        ptx_failed = false;
        ptx_ignore = (text_blob){null, 0};
        ptx_only = (text_blob){null, 0};
        ptx_reference_width = 0;
        ptx_maximum_word = 0;

        if ((flags & FILE_FLAG('w')) &&
            !pr_parse_positive(file_option_value(address_of taking, 'w'),
                               address_of ptx_width))
                return text_refuse(file_option_value(address_of taking, 'w'),
                                   "invalid line width", 1);

        if ((flags & FILE_FLAG('g')) &&
            !pr_parse_positive(file_option_value(address_of taking, 'g'),
                               address_of ptx_gap))
                return text_refuse(file_option_value(address_of taking, 'g'),
                                   "invalid gap width", 1);

        if (ptx_input_reference && ptx_custom_sentence)
                return text_refuse(null,
                                   "--references with --sentence-regexp is unsupported",
                                   1);

        if (flags & FILE_FLAG('F'))
                ptx_unescape((p8 address_to)ptx_truncation);

        if (ptx_custom_sentence)
                ptx_unescape((p8 address_to)ptx_sentence_pattern);

        if (ptx_word_pattern)
                ptx_unescape((p8 address_to)ptx_word_pattern);

        /* The overwhelmingly common explicit byte-C word expressions are
           character-set runs.  Lower them to the existing tokenizer map;
           sending every byte through the general backtracking matcher was
           more than twice GNU's cost on both target machines. */
        if (ptx_custom_word &&
            string_equals(ptx_word_pattern, "[a-z][a-z]*"))
        {
                ptx_custom_word = false;
                ptx_lower_word = true;
        }
        else if (ptx_custom_word &&
                 string_equals(ptx_word_pattern,
                               "[A-Za-z][A-Za-z]*"))
        {
                ptx_custom_word = false;
                ptx_alpha_word = true;
        }

        if ((flags & FILE_FLAG('i')) &&
            !text_blob_read(file_option_value(address_of taking, 'i'),
                           address_of ptx_ignore))
                return text_done(1);

        if ((flags & FILE_FLAG('o')) &&
            !text_blob_read(file_option_value(address_of taking, 'o'),
                           address_of ptx_only))
                return text_done(1);

        if (ptx_custom_word)
        {
                if (!regex_compile(ptx_word_pattern, false, ptx_fold, false,
                                   REGEX_POLICY_DEFAULT))
                        return text_refuse(ptx_word_pattern,
                                           "unsupported word expression", 1);
        }
        else if (ptx_lower_word || ptx_alpha_word)
        {
                for (positive character = 0; character < 256; character++)
                        fmt_word_bytes[character] =
                            ptx_alpha_word || ptx_fold
                                ? byte_is_alpha((p8)character)
                                : character >= 'a' && character <= 'z';
        }
        else if (flags & FILE_FLAG('b'))
        {
                text_blob breaks;

                if (!text_blob_read(file_option_value(address_of taking, 'b'),
                                   address_of breaks))
                        return text_done(1);

                memory_fill(fmt_word_bytes, 1, sizeof(fmt_word_bytes));

                for (positive at = 0; at < breaks.length; at++)
                        fmt_word_bytes[breaks.bytes[at]] = 0;
        }
        else
                for (positive character = 0; character < 256; character++)
                        fmt_word_bytes[character] =
                            byte_is_alpha((p8)character);

        ptx_file_count = text_input_count();
        ptx_files = (ptx_file address_to)text_arena_take(
            ptx_file_count * sizeof(ptx_file));

        if (!ptx_files)
                return text_done(1);

        for (positive input = 0; input < ptx_file_count; input++)
        {
                string_address name = text_file_name(input);

                if (name && !name[0])
                        name = null;

                ptx_files[input].name = name;

                if (!text_blob_read(name,
                                   address_of ptx_files[input].text))
                        return text_done(1);

                ptx_files[input].lines =
                    ptx_count_lines(address_of ptx_files[input].text);
        }

        if (ptx_custom_sentence && ptx_sentence_pattern[0] &&
            !regex_compile(ptx_sentence_pattern, false, ptx_fold, false,
                           REGEX_POLICY_DEFAULT))
                return text_refuse(ptx_sentence_pattern,
                                   "unsupported sentence expression", 1);

        ptx_context_count = ptx_plan_contexts(false);

        if (ptx_failed)
                return text_done(1);

        ptx_contexts = (ptx_context address_to)text_arena_take(
            ptx_context_count * sizeof(ptx_context));

        if (ptx_context_count && !ptx_contexts)
                return text_done(1);

        ptx_plan_contexts(true);

        if (ptx_custom_word &&
            !regex_compile(ptx_word_pattern, false, ptx_fold, false,
                           REGEX_POLICY_DEFAULT))
                return text_refuse(ptx_word_pattern,
                                   "unsupported word expression", 1);

        ptx_occurrence_count = ptx_scan_occurrences(false);
        ptx_occurrences = (ptx_occurrence address_to)text_arena_take(
            ptx_occurrence_count * sizeof(ptx_occurrence));
        ptx_order = (positive address_to)text_arena_take(
            ptx_occurrence_count * sizeof(positive));
        positive address_to spare = (positive address_to)text_arena_take(
            ptx_occurrence_count * sizeof(positive));

        if (ptx_occurrence_count &&
            (!ptx_occurrences || !ptx_order || !spare))
                return text_done(1);

        ptx_maximum_word = 0;
        ptx_scan_occurrences(true);

        for (positive at = 0; at < ptx_occurrence_count; at++)
                ptx_order[at] = at;

        if (ptx_occurrence_count > 1)
                ptx_order = array_merge_sort(ptx_order, spare,
                                             ptx_occurrence_count,
                                             ptx_compare);

        ptx_truncation_length = string_length(ptx_truncation);

        if (ptx_auto_reference)
        {
                for (positive file_index = 0; file_index < ptx_file_count;
                     file_index++)
                {
                        positive width =
                            (ptx_files[file_index].name
                                 ? string_length(ptx_files[file_index].name)
                                 : 0) +
                            1 + ptx_decimal_length(
                                    ptx_files[file_index].lines + 1);

                        if (width > ptx_reference_width)
                                ptx_reference_width = width;
                }
        }

        positive context_width = ptx_width;

        if ((ptx_auto_reference || ptx_input_reference) &&
            !ptx_right_reference)
        {
                positive used = ptx_reference_width + ptx_gap;
                context_width = used < context_width
                                    ? context_width - used
                                    : 0;
        }

        ptx_half_width = context_width / 2;
        ptx_before_width = ptx_half_width > ptx_gap
                               ? ptx_half_width - ptx_gap
                               : 0;
        ptx_keyafter_width = ptx_half_width;

        positive reserve = ptx_truncation_length * 2;
        ptx_before_width = reserve < ptx_before_width
                               ? ptx_before_width - reserve
                               : 0;
        ptx_keyafter_width = reserve < ptx_keyafter_width
                                 ? ptx_keyafter_width - reserve
                                 : 0;

        for (positive at = 0; at < ptx_occurrence_count; at++)
                ptx_output_one(ptx_occurrences + ptx_order[at]);

        return text_done(text_status);
}

/*
        Lists and tables.

        The byte spans below all point into text_arena (or the option vector),
        and the input blobs are filled by text_blob_read.  Planning is two
        cheap linear passes: the first counts rows and cells, the second lays
        down their descriptors.  No line copies, per-cell allocations, or
        extra I/O buffers are involved.

        Widths deliberately use C-locale bytes.  That is exact for ASCII and
        keeps malformed/package-generated input lossless; terminal-dependent
        wide-character and ANSI escape accounting is outside this applet's
        stated boundary and is rejected where it can be requested explicitly.
*/
typedef struct
{
        p8 address_to bytes;
        positive length;
} column_cell;

typedef struct
{
        positive first;
        positive count;
} column_row;

enum
{
        COLUMN_HIDDEN = 1,
        COLUMN_RIGHT = 2,
        COLUMN_NOEXTREME = 4,
        COLUMN_WRAP = 8,
        COLUMN_TRUNCATE = 16,
};

static text_blob address_to column_files;
static positive column_file_count;
static column_row address_to column_rows;
static positive column_row_count;
static column_cell address_to column_cells;
static positive column_cell_count;
static column_cell address_to column_names;
static positive column_name_count;
static positive column_count;
static positive column_row_at;
static positive column_cell_at;
static bool column_table;
static bool column_keep_empty;
static bool column_header_as_names;
static bool column_header_taken;
static bool column_custom_separator;
static string_address column_input_separator;

static bool column_is_separator(p8 character)
{
        if (!column_custom_separator)
                return character == ' ' || character == '\t';

        return string_first_of(column_input_separator, character) != null;
}

/* Split one table record exactly where util-linux's non-greedy -s tokenizer
   does.  The default blank tokenizer is greedy; an explicit separator keeps
   leading, adjacent and trailing empty fields. */
static positive column_fields(p8 address_to bytes, positive length,
                              column_cell address_to into, positive room)
{
        positive made = 0;
        positive at = 0;

        if (!length)
                return 0;

        if (!column_custom_separator)
        {
                while (at < length)
                {
                        while (at < length && column_is_separator(bytes[at]))
                                at++;

                        if (at == length)
                                break;

                        positive start = at;

                        while (at < length && !column_is_separator(bytes[at]))
                                at++;

                        if (into && made < room)
                                into[made] =
                                    (column_cell){bytes + start, at - start};

                        made++;
                }

                return made;
        }

        for (;;)
        {
                positive start = at;

                while (at < length && !column_is_separator(bytes[at]))
                        at++;

                if (into && made < room)
                        into[made] =
                            (column_cell){bytes + start, at - start};

                made++;

                if (at == length)
                        break;

                at++;

                if (at == length)
                {
                        if (into && made < room)
                                into[made] =
                                    (column_cell){bytes + at, 0};
                        made++;
                        break;
                }
        }

        return made;
}

static bool column_blank(p8 address_to bytes, positive length)
{
        for (positive at = 0; at < length; at++)
                if (!byte_is_space(bytes[at]))
                        return false;

        return true;
}

static fn column_accept_record(p8 address_to bytes, positive length,
                               bool fill)
{
        if (column_blank(bytes, length))
        {
                if (!column_keep_empty)
                        return;

                length = 0;
        }

        if (!column_table)
        {
                if (fill)
                {
                        column_rows[column_row_at] =
                            (column_row){column_cell_at, 1};
                        column_cells[column_cell_at] =
                            (column_cell){bytes, length};
                }

                column_row_at++;
                column_cell_at++;
                return;
        }

        positive fields = column_fields(bytes, length, null, 0);

        if (column_header_as_names && !column_header_taken)
        {
                column_header_taken = true;

                if (fill)
                        column_fields(bytes, length, column_names,
                                      column_name_count);
                else
                        column_name_count = fields;

                return;
        }

        if (!fill && fields > column_count)
                column_count = fields;

        if (fill)
        {
                column_rows[column_row_at] =
                    (column_row){column_cell_at, fields};
                column_fields(bytes, length,
                              fields ? column_cells + column_cell_at : null,
                              fields);
        }

        column_row_at++;
        column_cell_at += fields;
}

static fn column_scan(bool fill)
{
        column_row_at = 0;
        column_cell_at = 0;
        column_header_taken = false;

        for (positive file = 0; file < column_file_count; file++)
        {
                text_blob address_to blob = column_files + file;
                positive at = 0;

                while (at < blob->length)
                {
                        p8 address_to newline = memory_first_of(
                            blob->bytes + at, '\n', blob->length - at);
                        positive finish = newline
                                              ? (positive)(newline - blob->bytes)
                                              : blob->length;

                        column_accept_record(blob->bytes + at, finish - at,
                                             fill);

                        if (!newline)
                                break;

                        at = finish + 1;
                }
        }
}

static positive column_names_from_option(string_address names, bool fill,
                                         positive room)
{
        positive count = 0;
        positive at = 0;

        for (;;)
        {
                positive start = at;

                while (names[at] && names[at] != ',')
                        at++;

                if (fill && count < room)
                        column_names[count] = (column_cell){
                            (p8 address_to)names + start, at - start};

                count++;

                if (!names[at])
                        break;

                at++;
        }

        return count;
}

static bool column_span_unsigned(p8 address_to bytes, positive length,
                                 positive address_to answer)
{
        positive made = 0;

        if (!length)
                return false;

        for (positive at = 0; at < length; at++)
        {
                if (!byte_is_digit(bytes[at]) ||
                    made > (positive_max - (bytes[at] - '0')) / 10)
                        return false;

                made = made * 10 + bytes[at] - '0';
        }

        address_to answer = made;
        return true;
}

static bool column_name_equal(column_cell name, p8 address_to bytes,
                              positive length)
{
        return name.length == length &&
               !memory_compare(name.bytes, bytes, length);
}

static bool column_resolve(p8 address_to bytes, positive length,
                           positive address_to index)
{
        positive number;

        if (length == 2 && bytes[0] == '-' && bytes[1] == '1')
        {
                if (!column_count)
                        return false;

                address_to index = column_count - 1;
                return true;
        }

        if (column_span_unsigned(bytes, length, address_of number))
        {
                if (!number || number > column_count)
                        return false;

                address_to index = number - 1;
                return true;
        }

        for (positive at = 0; at < column_name_count; at++)
                if (column_name_equal(column_names[at], bytes, length))
                {
                        address_to index = at;
                        return true;
                }

        return false;
}

static bool column_apply_list(string_address list, p8 flag,
                              p8 address_to properties, bool unnamed)
{
        positive at = 0;

        while (list[at])
        {
                positive start = at;

                while (list[at] && list[at] != ',')
                        at++;

                positive length = at - start;
                p8 address_to item = (p8 address_to)list + start;

                if (length == 1 && item[0] == '0')
                        for (positive col = 0; col < column_count; col++)
                                properties[col] |= flag;
                else if (unnamed && length == 1 && item[0] == '-')
                        for (positive col = column_name_count;
                             col < column_count; col++)
                                properties[col] |= flag;
                else
                {
                        positive dash = 0;

                        while (dash < length && item[dash] != '-')
                                dash++;

                        positive low;
                        positive high;

                        if (dash && dash < length - 1 &&
                            column_span_unsigned(item, dash, address_of low) &&
                            column_span_unsigned(item + dash + 1,
                                                 length - dash - 1,
                                                 address_of high))
                        {
                                if (!low || low > high || high > column_count)
                                        return false;

                                for (positive col = low - 1; col < high; col++)
                                        properties[col] |= flag;
                        }
                        else
                        {
                                positive col;

                                if (!column_resolve(item, length,
                                                    address_of col))
                                        return false;

                                properties[col] |= flag;
                        }
                }

                if (list[at])
                        at++;
        }

        return true;
}

static bool column_make_order(string_address list,
                              positive address_to order,
                              p8 address_to properties,
                              positive address_to visible)
{
        positive made = 0;
        positive at = 0;

        if (list)
                while (list[at])
                {
                        positive start = at;
                        positive col;

                        while (list[at] && list[at] != ',')
                                at++;

                        if (!column_resolve((p8 address_to)list + start,
                                            at - start, address_of col))
                                return false;

                        bool repeated = false;

                        for (positive prior = 0; prior < made; prior++)
                                if (order[prior] == col)
                                        repeated = true;

                        if (!repeated && !(properties[col] & COLUMN_HIDDEN))
                                order[made++] = col;

                        if (list[at])
                                at++;
                }

        for (positive col = 0; col < column_count; col++)
        {
                bool repeated = false;

                for (positive prior = 0; prior < made; prior++)
                        if (order[prior] == col)
                                repeated = true;

                if (!repeated && !(properties[col] & COLUMN_HIDDEN))
                        order[made++] = col;
        }

        address_to visible = made;
        return true;
}

static column_cell column_row_cell(column_row address_to row, positive col)
{
        if (col >= row->count)
                return (column_cell){null, 0};

        return column_cells[row->first + col];
}

static fn column_plain(bool fill_rows, bool spaces, positive spacing,
                       positive width)
{
        if (!column_row_count)
                return;

        positive longest = 0;

        for (positive at = 0; at < column_row_count; at++)
                if (column_cells[column_rows[at].first].length > longest)
                        longest = column_cells[column_rows[at].first].length;

        if (!width || longest >= width)
        {
                for (positive at = 0; at < column_row_count; at++)
                {
                        column_cell cell = column_cells[column_rows[at].first];
                        text_put(cell.bytes, cell.length);
                        text_put_character('\n');
                }

                return;
        }

        positive stride = spaces ? longest + spacing
                                  : (longest + 8) & ~(positive)7;

        if (!stride)
                stride = spaces ? 1 : 8;

        positive columns = width / stride;
        positive remains = width % stride;

        if (!columns)
                columns = 1;

        if (spaces && remains <= positive_max - spacing &&
            remains + spacing >= stride)
                columns++;

        positive rows = (column_row_count + columns - 1) / columns;

        for (positive row = 0; row < rows; row++)
        {
                positive output_column = 0;

                for (positive col = 0; col < columns; col++)
                {
                        positive entry = fill_rows ? row * columns + col
                                                   : row + col * rows;

                        if (entry >= column_row_count)
                                break;

                        column_cell cell =
                            column_cells[column_rows[entry].first];
                        text_put(cell.bytes, cell.length);
                        output_column += cell.length;

                        positive next = fill_rows ? entry + 1
                                                  : entry + rows;

                        if (col + 1 == columns || next >= column_row_count)
                                break;

                        positive target = (col + 1) * stride;

                        if (spaces)
                        {
                                if (output_column < target)
                                        text_tab_repeat_character(
                                            ' ', target - output_column);
                                output_column = target;
                        }
                        else
                                while (((output_column + 8) & ~(positive)7) <=
                                       target)
                                {
                                        text_put_character('\t');
                                        output_column =
                                            (output_column + 8) & ~(positive)7;
                                }
                }

                text_put_character('\n');
        }
}

static fn column_json_string(column_cell value, bool lower)
{
        text_put_character('"');

        for (positive at = 0; at < value.length; at++)
        {
                p8 character = value.bytes[at];

                if (lower && character >= 'A' && character <= 'Z')
                        character += 'a' - 'A';

                if (character == '"' || character == '\\')
                {
                        text_put_character('\\');
                        text_put_character(character);
                }
                else if (character == '\b')
                        text_put_string("\\b");
                else if (character == '\f')
                        text_put_string("\\f");
                else if (character == '\n')
                        text_put_string("\\n");
                else if (character == '\r')
                        text_put_string("\\r");
                else if (character == '\t')
                        text_put_string("\\t");
                else if (character < 32)
                {
                        static const p8 hex[] = "0123456789abcdef";
                        p8 escaped[6] = {'\\', 'u', '0', '0',
                                         hex[character >> 4],
                                         hex[character & 15]};
                        text_put(escaped, sizeof(escaped));
                }
                else
                        text_put_character(character);
        }

        text_put_character('"');
}

static bool column_json(string_address name, positive address_to order,
                        positive visible)
{
        if (!column_row_count)
                return true;

        for (positive at = 0; at < visible; at++)
                if (order[at] >= column_name_count ||
                    !column_names[order[at]].length)
                        return false;

        text_put_string("{\n   ");
        column_json_string((column_cell){(p8 address_to)name,
                                         string_length(name)}, false);
        text_put_string(": [\n");

        for (positive row_at = 0; row_at < column_row_count; row_at++)
        {
                column_row address_to row = column_rows + row_at;

                if (!row_at)
                        text_put_string("      {\n");

                for (positive shown = 0; shown < visible; shown++)
                {
                        positive col = order[shown];
                        column_cell cell = column_row_cell(row, col);
                        text_put_string("         ");
                        column_json_string(column_names[col], true);
                        text_put_string(": ");

                        if (!cell.length)
                                text_put_string("null");
                        else
                                column_json_string(cell, false);

                        text_put_string(shown + 1 < visible ? ",\n" : "\n");
                }

                text_put_string(row_at + 1 < column_row_count
                                    ? "      },{\n"
                                    : "      }\n");
        }

        text_put_string("   ]\n}\n");
        return true;
}

static positive column_cell_part(column_cell cell, p8 property,
                                 positive width, positive part,
                                 column_cell address_to answer)
{
        if (property & COLUMN_TRUNCATE)
        {
                if (part)
                        return 0;

                answer->bytes = cell.bytes;
                answer->length = min(cell.length, width);
                return answer->length;
        }

        if (property & COLUMN_WRAP)
        {
                positive start = part * width;

                if (start >= cell.length)
                        return 0;

                answer->bytes = cell.bytes + start;
                answer->length = min(width, cell.length - start);
                return answer->length;
        }

        if (part)
                return 0;

        address_to answer = cell;
        return cell.length;
}

static fn column_table_line(column_row address_to row, bool header,
                            positive address_to order, positive visible,
                            p8 address_to properties,
                            positive address_to widths,
                            string_address separator)
{
        positive parts = 1;

        for (positive shown = 0; shown < visible; shown++)
        {
                positive col = order[shown];
                column_cell cell = header
                                       ? (col < column_name_count
                                              ? column_names[col]
                                              : (column_cell){null, 0})
                                       : column_row_cell(row, col);

                if ((properties[col] & COLUMN_WRAP) && widths[col] &&
                    cell.length)
                {
                        positive needed =
                            (cell.length + widths[col] - 1) / widths[col];
                        if (needed > parts)
                                parts = needed;
                }
        }

        for (positive part = 0; part < parts; part++)
        {
                for (positive shown = 0; shown < visible; shown++)
                {
                        positive col = order[shown];
                        column_cell whole = header
                                                ? (col < column_name_count
                                                       ? column_names[col]
                                                       : (column_cell){null, 0})
                                                : column_row_cell(row, col);
                        column_cell cell = {null, 0};
                        positive length = column_cell_part(
                            whole, properties[col], widths[col], part,
                            address_of cell);
                        positive pad = length < widths[col]
                                           ? widths[col] - length
                                           : 0;

                        if ((properties[col] & COLUMN_RIGHT) &&
                            (length || shown + 1 < visible))
                                text_tab_repeat_character(' ', pad);

                        text_put(cell.bytes, cell.length);

                        if (!(properties[col] & COLUMN_RIGHT) &&
                            shown + 1 < visible)
                                text_tab_repeat_character(' ', pad);

                        if (shown + 1 < visible)
                                text_put_string(separator);
                }

                text_put_character('\n');
        }
}

static fn column_table_output(bool noheadings, positive width,
                              string_address separator,
                              positive address_to order, positive visible,
                              p8 address_to properties)
{
        /* Names describe a table; they do not by themselves create one. */
        if (!column_row_count)
                return;

        positive address_to widths = (positive address_to)text_arena_take(
            column_count * sizeof(positive));
        positive address_to second = (positive address_to)text_arena_take(
            column_count * sizeof(positive));

        if (column_count && (!widths || !second))
                return;

        memory_fill(widths, 0, column_count * sizeof(positive));
        memory_fill(second, 0, column_count * sizeof(positive));

        if (!noheadings)
                for (positive col = 0; col < column_name_count; col++)
                        widths[col] = column_names[col].length;

        for (positive row_at = 0; row_at < column_row_count; row_at++)
                for (positive col = 0;
                     col < column_rows[row_at].count && col < column_count;
                     col++)
                {
                        positive length =
                            column_cells[column_rows[row_at].first + col].length;

                        if (length > widths[col])
                        {
                                second[col] = widths[col];
                                widths[col] = length;
                        }
                        else if (length > second[col])
                                second[col] = length;
                }

        if (visible && width)
        {
                positive total = string_length(separator) * (visible - 1);

                for (positive shown = 0; shown < visible; shown++)
                        total += widths[order[shown]];

                if (total > width)
                {
                        /* libsmartcols marks the last visible column this way
                           by default.  Ignore its one extreme only when width
                           pressure exists; an overlong final value remains
                           lossless, exactly as the native renderer does. */
                        if (!(properties[order[visible - 1]] &
                              (COLUMN_WRAP | COLUMN_TRUNCATE)))
                                properties[order[visible - 1]] |=
                                    COLUMN_NOEXTREME;

                        for (positive shown = 0; shown < visible; shown++)
                        {
                                positive col = order[shown];

                                if ((properties[col] & COLUMN_NOEXTREME) &&
                                    second[col] < widths[col])
                                {
                                        positive replacement = second[col]
                                                                   ? second[col]
                                                                   : 1;
                                        total -= widths[col] - replacement;
                                        widths[col] = replacement;
                                }
                        }

                        while (total > width)
                        {
                                positive chosen = column_count;

                                for (positive shown = 0; shown < visible; shown++)
                                {
                                        positive col = order[shown];

                                        if ((properties[col] &
                                             (COLUMN_WRAP | COLUMN_TRUNCATE)) &&
                                            widths[col] > 1 &&
                                            (chosen == column_count ||
                                             widths[col] > widths[chosen]))
                                                chosen = col;
                                }

                                if (chosen == column_count)
                                        break;

                                widths[chosen]--;
                                total--;
                        }
                }
        }

        if (column_name_count && !noheadings)
                column_table_line(null, true, order, visible, properties,
                                  widths, separator);

        for (positive row = 0; row < column_row_count; row++)
                column_table_line(column_rows + row, false, order, visible,
                                  properties, widths, separator);
}

static const file_long column_longs[] = {
    {(string_address)"columns", 'c'},
    {(string_address)"color", 'q'},
    {(string_address)"fillrows", 'x'},
    {(string_address)"input-separator", 's'},
    {(string_address)"json", 'J'},
    {(string_address)"keep-empty-lines", 'L'},
    {(string_address)"output-separator", 'o'},
    {(string_address)"output-width", 'c'},
    {(string_address)"separator", 's'},
    {(string_address)"table", 't'},
    {(string_address)"table-colorscheme", 'Q'},
    {(string_address)"table-columns", 'N'},
    {(string_address)"table-column", 'C'},
    {(string_address)"table-columns-limit", 'l'},
    {(string_address)"table-hide", 'H'},
    {(string_address)"table-name", 'n'},
    {(string_address)"table-maxout", 'm'},
    {(string_address)"table-noextreme", 'E'},
    {(string_address)"table-noheadings", 'd'},
    {(string_address)"table-order", 'O'},
    {(string_address)"table-right", 'R'},
    {(string_address)"table-truncate", 'T'},
    {(string_address)"table-wrap", 'W'},
    {(string_address)"table-empty-lines", 'L'},
    {(string_address)"table-header-repeat", 'e'},
    {(string_address)"table-header-as-columns", 'K'},
    {(string_address)"tree", 'r'},
    {(string_address)"tree-id", 'i'},
    {(string_address)"tree-parent", 'p'},
    {(string_address)"use-spaces", 'S'},
    {(string_address)"wrap-separator", 'G'},
    {null, 0},
};

static b32 text_column()
{
        file_taking taking = {
            .program = (string_address)"column",
            .allowed = (string_address)"CcdEeHiJKlLNnmOopRrSsTtWx",
            .valued = (string_address)"CcEHilNnOopQRrSsTWG",
            .long_optional = (string_address)"q",
            .longs = column_longs,
            .operand = text_file_add,
        };

        text_begin("column");
        text_arena_used = 0;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        positive flags = taking.flags;

        if (flags & (FILE_FLAG('C') | FILE_FLAG('e') | FILE_FLAG('l') |
                     FILE_FLAG('m') |
                     FILE_FLAG('r') | FILE_FLAG('i') | FILE_FLAG('p') |
                     FILE_FLAG('q') | FILE_FLAG('Q') | FILE_FLAG('G')))
                return text_refuse(null,
                                   "column properties, tree, color, header-repeat and custom wrap parsing are unsupported",
                                   1);

        column_table = (flags & (FILE_FLAG('t') | FILE_FLAG('J') |
                                 FILE_FLAG('K'))) != 0;
        bool fill_rows = (flags & FILE_FLAG('x')) != 0;

        if (fill_rows && column_table)
                return text_refuse(null,
                                   "--fillrows and --table are mutually exclusive",
                                   1);

        positive table_options = FILE_FLAG('N') | FILE_FLAG('n') |
                                 FILE_FLAG('O') | FILE_FLAG('H') |
                                 FILE_FLAG('E') | FILE_FLAG('R') |
                                 FILE_FLAG('T') | FILE_FLAG('W') |
                                 FILE_FLAG('d') | FILE_FLAG('l');

        if (!column_table && (flags & table_options))
                return text_refuse(null,
                                   "option --table required for all --table-*",
                                   1);

        if ((flags & FILE_FLAG('N')) && (flags & FILE_FLAG('K')))
                return text_refuse(null,
                                   "--table-columns and --table-header-as-columns are mutually exclusive",
                                   1);

        positive width = 80;
        string_address width_option = file_option_value(address_of taking, 'c');

        if (width_option)
        {
                if (string_equals(width_option, "unlimited"))
                        width = 0;
                else if (!text_unsigned_option(width_option, false,
                                               address_of width))
                        return text_refuse(width_option,
                                           "invalid columns argument", 1);
        }

        bool spaces = (flags & FILE_FLAG('S')) != 0;
        positive spacing = 0;

        if (spaces &&
            !text_unsigned_option(file_option_value(address_of taking, 'S'),
                                  false, address_of spacing))
                return text_refuse(file_option_value(address_of taking, 'S'),
                                   "invalid spaces argument", 1);

        column_keep_empty = (flags & FILE_FLAG('L')) != 0;
        column_header_as_names = (flags & FILE_FLAG('K')) != 0;
        column_custom_separator = (flags & FILE_FLAG('s')) != 0;
        column_input_separator = column_custom_separator
                                     ? file_option_value(address_of taking, 's')
                                     : (string_address)" \t";

        if ((flags & FILE_FLAG('J')) && !(flags & FILE_FLAG('N')) &&
            !column_header_as_names)
                return text_refuse(null,
                                   "option --table-columns or --table-column required for --json",
                                   1);

        column_file_count = text_input_count();
        column_files = (text_blob address_to)text_arena_take(
            column_file_count * sizeof(text_blob));

        if (!column_files)
                return text_done(1);

        for (positive file = 0; file < column_file_count; file++)
        {
                column_files[file] = (text_blob){null, 0};
                text_blob_read(text_file_name((b32)file),
                               column_files + file);
        }

        column_row_count = 0;
        column_cell_count = 0;
        column_name_count = 0;
        column_count = 0;

        if (flags & FILE_FLAG('N'))
        {
                column_name_count = column_names_from_option(
                    file_option_value(address_of taking, 'N'), false, 0);
                column_count = column_name_count;
        }

        column_scan(false);
        column_row_count = column_row_at;
        column_cell_count = column_cell_at;

        if (column_name_count > column_count)
                column_count = column_name_count;

        column_rows = (column_row address_to)text_arena_take(
            column_row_count * sizeof(column_row));
        column_cells = (column_cell address_to)text_arena_take(
            column_cell_count * sizeof(column_cell));
        column_names = (column_cell address_to)text_arena_take(
            column_count * sizeof(column_cell));

        if ((column_row_count && !column_rows) ||
            (column_cell_count && !column_cells) ||
            (column_count && !column_names))
                return text_done(1);

        memory_fill(column_names, 0, column_count * sizeof(column_cell));

        if (flags & FILE_FLAG('N'))
                column_names_from_option(
                    file_option_value(address_of taking, 'N'), true,
                    column_name_count);

        column_scan(true);

        if (!column_table)
        {
                column_plain(fill_rows, spaces, spacing, width);
                return text_done(text_status);
        }

        p8 address_to properties = (p8 address_to)text_arena_take(column_count);
        positive address_to order = (positive address_to)text_arena_take(
            column_count * sizeof(positive));

        if (column_count && (!properties || !order))
                return text_done(1);

        memory_fill(properties, 0, column_count);

#define COLUMN_LIST(letter, property, unnamed)                              \
        if ((flags & FILE_FLAG(letter)) &&                                  \
            !column_apply_list(file_option_value(address_of taking, letter),\
                               property, properties, unnamed))              \
                return text_refuse(file_option_value(address_of taking,     \
                                                      letter),              \
                                   "undefined column name", 1)

        COLUMN_LIST('H', COLUMN_HIDDEN, true);
        COLUMN_LIST('E', COLUMN_NOEXTREME, false);
        COLUMN_LIST('R', COLUMN_RIGHT, false);
        COLUMN_LIST('T', COLUMN_TRUNCATE, false);
        COLUMN_LIST('W', COLUMN_WRAP, false);
#undef COLUMN_LIST

        positive visible;

        if (!column_make_order((flags & FILE_FLAG('O'))
                                   ? file_option_value(address_of taking, 'O')
                                   : null,
                               order, properties, address_of visible))
                return text_refuse(file_option_value(address_of taking, 'O'),
                                   "undefined column name", 1);

        if (flags & FILE_FLAG('J'))
        {
                string_address name = (flags & FILE_FLAG('n'))
                                          ? file_option_value(address_of taking,
                                                              'n')
                                          : (string_address)"table";

                if (!column_json(name, order, visible))
                        return text_refuse(null,
                                           "for JSON every visible column requires a name",
                                           1);
        }
        else
                column_table_output((flags & FILE_FLAG('d')) != 0, width,
                                    (flags & FILE_FLAG('o'))
                                        ? file_option_value(address_of taking,
                                                            'o')
                                        : (string_address)"  ",
                                    order, visible, properties);

        return text_done(text_status);
}

/*
        Terminal-column filters.

        col, colcrt and colrm have one byte walker.  Their policies differ at
        the event boundary -- col retains positioned overstrikes, colcrt
        keeps the historical 132-column CRT planes, and colrm streams a
        clipping state -- but no command grows a private reader or scanner.
        C-locale bytes occupy one cell; controls named by the old terminal
        protocol move the cursor and every other byte is either deliberately
        dropped or, for col -p, preserved with zero width.
*/
enum
{
        TERMINAL_COL,
        TERMINAL_COLCRT,
        TERMINAL_COLRM,
        TERMINAL_UL,
};

typedef struct
{
        /* text_arena caps one input below 192 MiB, so byte-C line and column
           positions (including two half-lines per newline) fit signed and
           unsigned 32-bit fields.  Keeping events at 12 rather than 24 bytes
           is the difference between cache-resident nroff and allocator-like
           traffic on ordinary manuals. */
        b32 line;
        p32 column;
        p8 character;
        p8 set;
        p8 width;
} terminal_event;

typedef struct
{
        p8 mode;

        /* col */
        bipolar line;
        bipolar maximum_line;
        bipolar minimum_line;
        positive column;
        positive events;
        bool fine;
        bool pass;
        bool no_backspaces;
        bool compress;
        bool ordered;
        bool have_event;
        bipolar prior_line;
        positive prior_column;
        p8 recent_width;
        p8 character_set;
        terminal_event address_to event;

        /* colcrt */
        positive crt_column;
        bool crt_no_underlining;
        bool crt_half_lines;
        bool crt_need_under;
        bool crt_print_newline;
        bool crt_discard;

        /* colrm */
        positive remove_first;
        positive remove_last;
        positive remove_column;
        p8 remove_phase;
        bool remove_padded;

        /* ul: text_line is the glyph plane and text_record_hold is its
           attribute plane. They are idle while the arena-backed input blob
           is scanned, so underlining adds policy, not another line store. */
        positive ul_column;
        positive ul_max_column;
        positive ul_plane_column;
        positive ul_up_line;
        bipolar ul_half_position;
        p8 ul_mode;
        p8 ul_current_mode;
        p8 ul_terminal;
        bool ul_indicated;
        bool ul_failed;
} terminal_state;

static terminal_event address_to terminal_events;
static positive address_to terminal_order;

static PURE HOT bipolar terminal_event_compare(positive left, positive right)
{
        terminal_event address_to one = terminal_events + left;
        terminal_event address_to two = terminal_events + right;

        if (one->line != two->line)
                return one->line < two->line ? -1 : 1;

        if (one->column != two->column)
                return one->column < two->column ? -1 : 1;

        return 0;
}

static fn terminal_col_event(terminal_state address_to state, p8 character,
                             p8 width, bool fill)
{
        bipolar line = state->line;

        if (!state->fine && (line & 1))
                line++;

        if (line < state->minimum_line)
                state->minimum_line = line;

        if (state->have_event &&
            (line < state->prior_line ||
             (line == state->prior_line &&
              state->column < state->prior_column)))
                state->ordered = false;

        state->have_event = true;
        state->prior_line = line;
        state->prior_column = state->column;

        if (fill)
                state->event[state->events] = (terminal_event){
                    line, state->column, character, state->character_set,
                    width};

        state->events++;
        state->recent_width = width;

        if (width && width != 255)
                state->column += width;
}

static positive terminal_crt_length(p8 address_to line, positive room)
{
        positive length = 0;

        while (length < room && line[length])
                length++;

        while (length && byte_is_space(line[length - 1]))
                length--;

        return length;
}

static fn terminal_crt_clear()
{
        memory_fill(text_line, 0, 133);
        memory_fill(text_record_hold, ' ', 132);
        text_record_hold[132] = 0;
}

static fn terminal_crt_output(terminal_state address_to state, bool eof)
{
        if (eof)
                state->crt_print_newline = false;

        positive length = terminal_crt_length(text_line, 132);
        text_put(text_line, length);

        if (state->crt_print_newline)
                text_put_character('\n');

        if (!state->crt_half_lines && !state->crt_no_underlining)
                state->crt_print_newline = false;

        memory_fill(text_line, 0, 133);

        if (state->crt_need_under)
        {
                state->crt_need_under = false;
                positive stop = min(state->crt_column, (positive)132);
                text_record_hold[stop] = 0;
                length = terminal_crt_length(text_record_hold, stop);
                text_put(text_record_hold, length);
                text_put_character('\n');
                memory_fill(text_record_hold, ' ', 132);
                text_record_hold[132] = 0;
        }
        else if (state->crt_half_lines && state->crt_column)
                text_put_character('\n');
}

static fn terminal_crt_rub(terminal_state address_to state, positive count)
{
        positive col = state->crt_column;

        while (count && col)
        {
                if (col < 132)
                {
                        text_line[col] = 0;
                        text_record_hold[col] = ' ';
                }

                count--;
                col--;
        }

        /* The historical loop increments once after consuming ESC plus its
           command byte. */
        state->crt_column = col + 1;
}

enum
{
        TERMINAL_UL_NORMAL = 0,
        TERMINAL_UL_ALTERNATIVE = 1,
        TERMINAL_UL_SUPER = 2,
        TERMINAL_UL_SUB = 4,
        TERMINAL_UL_UNDERLINE = 8,
        TERMINAL_UL_BOLD = 16,
};

enum
{
        TERMINAL_UL_DUMB,
        TERMINAL_UL_ANSI,
        TERMINAL_UL_XTERM,
        TERMINAL_UL_LINUX,
        TERMINAL_UL_VT100,
};

static fn terminal_ul_clear(terminal_state address_to state)
{
        if (state->ul_max_column)
        {
                memory_fill(text_line, 0, state->ul_max_column);
                memory_fill(text_record_hold, 0, state->ul_max_column);
        }

        state->ul_column = 0;
        state->ul_max_column = 0;
        state->ul_mode &= TERMINAL_UL_ALTERNATIVE;
}

static bool terminal_ul_set_column(terminal_state address_to state,
                                   positive column)
{
        if (column > TEXT_LINE_MAX)
        {
                text_error(null, "line too long");
                state->ul_failed = true;
                return false;
        }

        state->ul_column = column;

        if (state->ul_plane_column < column)
        {
                memory_fill(text_line + state->ul_plane_column, 0,
                            column - state->ul_plane_column);
                memory_fill(text_record_hold + state->ul_plane_column, 0,
                            column - state->ul_plane_column);
                state->ul_plane_column = column;
        }

        if (state->ul_max_column < column)
                state->ul_max_column = column;

        return true;
}

/* util-linux's need_column is subtly different from cursor movement: every
   printable byte makes its right edge the new output edge, including after a
   carriage return. Bytes already beyond that edge remain in the plane and
   can become visible again after a later tab. */
static bool terminal_ul_need_column(terminal_state address_to state,
                                    positive column)
{
        if (column > TEXT_LINE_MAX)
        {
                text_error(null, "line too long");
                state->ul_failed = true;
                return false;
        }

        if (state->ul_plane_column < column)
        {
                memory_fill(text_line + state->ul_plane_column, 0,
                            column - state->ul_plane_column);
                memory_fill(text_record_hold + state->ul_plane_column, 0,
                            column - state->ul_plane_column);
                state->ul_plane_column = column;
        }

        state->ul_max_column = column;
        return true;
}

static fn terminal_ul_cursor(terminal_state address_to state, bool up)
{
        if (state->ul_terminal == TERMINAL_UL_DUMB)
                return;

        text_put_string(up ? (string_address)"\033[A"
                           : (string_address)"\033[C");
}

static fn terminal_ul_mode(terminal_state address_to state, p8 mode)
{
        if (state->ul_current_mode == mode)
                return;

        if (state->ul_indicated ||
            state->ul_terminal == TERMINAL_UL_DUMB)
        {
                state->ul_current_mode = mode;
                return;
        }

        /* terminfo's sgr modes do not compose portably. ul returns to normal
           between two non-normal modes and then enters the next one. */
        if (state->ul_current_mode && mode)
                terminal_ul_mode(state, TERMINAL_UL_NORMAL);

        if (!mode)
        {
                if (state->ul_current_mode == TERMINAL_UL_UNDERLINE)
                {
                        if (state->ul_terminal == TERMINAL_UL_XTERM ||
                            state->ul_terminal == TERMINAL_UL_LINUX)
                                text_put_string((string_address)"\033[24m");
                        else
                                text_put_string((string_address)"\033[m");
                }
                else if (state->ul_terminal == TERMINAL_UL_ANSI)
                        text_put_string((string_address)"\033[0;10m");
                else if (state->ul_terminal == TERMINAL_UL_XTERM)
                        text_put_string((string_address)"\033(B\033[m");
                else
                        text_put_string((string_address)"\033[m\017");
        }
        else if (mode == TERMINAL_UL_UNDERLINE)
                text_put_string((string_address)"\033[4m");
        else if (mode == TERMINAL_UL_BOLD)
                text_put_string((string_address)"\033[1m");
        else if (mode == TERMINAL_UL_ALTERNATIVE)
                text_put_string((string_address)"\033[7m");
        else if (mode == TERMINAL_UL_SUPER)
        {
                text_put_string((string_address)"\033[4m");
                text_put_string(state->ul_terminal == TERMINAL_UL_XTERM ||
                                        state->ul_terminal == TERMINAL_UL_LINUX
                                    ? (string_address)"\033[2m"
                                    : (string_address)"\033[7m");
        }
        else if (mode == TERMINAL_UL_SUB)
                text_put_string(state->ul_terminal == TERMINAL_UL_XTERM ||
                                        state->ul_terminal == TERMINAL_UL_LINUX
                                    ? (string_address)"\033[2m"
                                    : (string_address)"\033[7m");
        else
                text_put_string((string_address)"\033[7m");

        state->ul_current_mode = mode;
}

static p8 terminal_ul_indicator(p8 mode)
{
        switch (mode)
        {
        case TERMINAL_UL_NORMAL: return ' ';
        case TERMINAL_UL_ALTERNATIVE: return 'g';
        case TERMINAL_UL_SUPER: return '^';
        case TERMINAL_UL_SUB: return 'v';
        case TERMINAL_UL_UNDERLINE: return '_';
        case TERMINAL_UL_BOLD: return '!';
        default: return 'X';
        }
}

static fn terminal_ul_flush(terminal_state address_to state)
{
        p8 last_mode = TERMINAL_UL_NORMAL;
        bool had_mode = false;

        for (positive column = 0; column < state->ul_max_column; column++)
        {
                p8 mode = text_record_hold[column];

                if (mode != last_mode)
                {
                        had_mode = true;
                        terminal_ul_mode(state, mode);
                        last_mode = mode;
                }

                if (text_line[column])
                        text_put_character(text_line[column]);
                else if (state->ul_up_line)
                        terminal_ul_cursor(state, false);
                else
                        text_put_character(' ');
        }

        if (last_mode)
                terminal_ul_mode(state, TERMINAL_UL_NORMAL);

        text_put_character('\n');

        if (state->ul_indicated && had_mode)
        {
                positive stop = state->ul_max_column;

                while (stop &&
                       terminal_ul_indicator(text_record_hold[stop - 1]) == ' ')
                        stop--;

                for (positive column = 0; column < stop; column++)
                        text_put_character(terminal_ul_indicator(
                            text_record_hold[column]));

                text_put_character('\n');
        }

        if (state->ul_up_line)
                state->ul_up_line--;

        terminal_ul_clear(state);
}

static fn terminal_ul_forward(terminal_state address_to state)
{
        positive column = state->ul_column;
        positive maximum = state->ul_max_column;

        terminal_ul_flush(state);
        state->ul_column = column;
        state->ul_max_column = maximum;
}

static fn terminal_ul_reverse(terminal_state address_to state)
{
        state->ul_up_line++;
        terminal_ul_forward(state);
        terminal_ul_cursor(state, true);
        terminal_ul_cursor(state, true);
        state->ul_up_line++;
}

static fn terminal_ul_escape(terminal_state address_to state, p8 command)
{
        if (command == '8')
        {
                if (state->ul_half_position > 0)
                {
                        state->ul_mode &= (p8)~TERMINAL_UL_SUB;
                        state->ul_half_position--;
                }
                else if (!state->ul_half_position)
                {
                        state->ul_mode |= TERMINAL_UL_SUPER;
                        state->ul_half_position--;
                }
                else
                {
                        state->ul_half_position = 0;
                        terminal_ul_reverse(state);
                }
        }
        else if (command == '9')
        {
                if (state->ul_half_position < 0)
                {
                        state->ul_mode &= (p8)~TERMINAL_UL_SUPER;
                        state->ul_half_position++;
                }
                else if (!state->ul_half_position)
                {
                        state->ul_mode |= TERMINAL_UL_SUB;
                        state->ul_half_position++;
                }
                else
                {
                        state->ul_half_position = 0;
                        terminal_ul_forward(state);
                }
        }
        else if (command == '7')
                terminal_ul_reverse(state);
        else
        {
                text_error(null, "unknown escape sequence in input");
                state->ul_failed = true;
        }
}

static fn terminal_ul_byte(terminal_state address_to state, p8 character)
{
        if (character == '\b')
        {
                if (state->ul_column)
                        state->ul_column--;
                return;
        }
        if (character == '\t')
        {
                terminal_ul_set_column(state,
                                       (state->ul_column + 8) & ~(positive)7);
                return;
        }
        if (character == '\r')
        {
                state->ul_column = 0;
                return;
        }
        if (character == 016)
        {
                state->ul_mode |= TERMINAL_UL_ALTERNATIVE;
                return;
        }
        if (character == 017)
        {
                state->ul_mode &= (p8)~TERMINAL_UL_ALTERNATIVE;
                return;
        }
        if (character == '\n' || character == '\f')
        {
                terminal_ul_flush(state);

                if (character == '\f')
                        text_put_character('\f');

                return;
        }
        if (character == ' ')
        {
                terminal_ul_set_column(state, state->ul_column + 1);
                return;
        }
        if (!byte_is_printable(character))
                return;

        if (state->ul_column >= TEXT_LINE_MAX)
        {
                text_error(null, "line too long");
                state->ul_failed = true;
                return;
        }

        positive column = state->ul_column;

        if (character == '_')
        {
                if (!terminal_ul_set_column(state, column + 1))
                        return;

                if (text_line[column])
                        text_record_hold[column] |=
                            TERMINAL_UL_UNDERLINE | state->ul_mode;
                else
                {
                        text_line[column] = '_';
                        /* A literal underscore is data, not an attributed
                           glyph. The active mode joins it only if a later
                           overstrike turns it into underlining. */
                        text_record_hold[column] = TERMINAL_UL_NORMAL;
                }
        }
        else
        {
                if (!terminal_ul_need_column(state, column + 1))
                        return;

                if (!text_line[column])
                {
                        text_line[column] = character;
                        text_record_hold[column] = state->ul_mode;
                }
                else if (text_line[column] == '_')
                {
                        text_line[column] = character;
                        text_record_hold[column] |=
                            TERMINAL_UL_UNDERLINE | state->ul_mode;
                }
                else if (text_line[column] == character)
                        text_record_hold[column] |=
                            TERMINAL_UL_BOLD | state->ul_mode;
                else
                        text_record_hold[column] = state->ul_mode;

                state->ul_column = column + 1;
        }
}

static positive terminal_byte_width(p8 character, positive column)
{
        if (character == '\t')
                return ((column + 8) & ~(positive)7) - column;

        if (character == '\b')
                return column ? (positive)-1 : 0;

        return byte_is_printable(character) ? 1 : 0;
}

static fn terminal_colrm_byte(terminal_state address_to state, p8 character)
{
        if (character == '\n')
        {
                text_put_character(character);
                state->remove_column = 0;
                state->remove_phase = 0;
                state->remove_padded = false;
                return;
        }

        if (!state->remove_first)
        {
                text_put_character(character);
                return;
        }

        if (state->remove_phase == 2)
        {
                if (!state->remove_padded &&
                    state->remove_last < state->remove_column)
                {
                        text_tab_repeat_character(
                            ' ', state->remove_column - state->remove_last);
                        state->remove_padded = true;
                }

                text_put_character(character);
                return;
        }

        positive before = state->remove_column;
        positive width = terminal_byte_width(character, before);

        if (width == (positive)-1)
                state->remove_column = before ? before - 1 : 0;
        else
                state->remove_column += width;

        if (!state->remove_phase)
        {
                if (state->remove_column < state->remove_first)
                {
                        text_put_character(character);
                        return;
                }

                if (state->remove_first > before + 1)
                        text_tab_repeat_character(
                            ' ', state->remove_first - before - 1);

                state->remove_phase = 1;
        }

        if (state->remove_last &&
            state->remove_column >= state->remove_last)
                state->remove_phase = 2;
}

/* The sole scanner for the family.  ESC consumes its command byte here, so a
   refill boundary cannot make any renderer interpret it twice. */
static fn terminal_scan(text_blob address_to blob,
                        terminal_state address_to state, bool fill)
{
        positive at = 0;

        while (at < blob->length)
        {
                p8 character = blob->bytes[at++];

                if (state->mode == TERMINAL_UL)
                {
                        if (character == 033)
                        {
                                if (at >= blob->length)
                                {
                                        text_error(null,
                                                   "unknown escape sequence in input");
                                        state->ul_failed = true;
                                        break;
                                }

                                terminal_ul_escape(state, blob->bytes[at++]);
                        }
                        else
                                terminal_ul_byte(state, character);

                        if (state->ul_failed)
                                break;

                        continue;
                }

                if (state->mode == TERMINAL_COLRM)
                {
                        terminal_colrm_byte(state, character);
                        continue;
                }

                if (state->mode == TERMINAL_COLCRT)
                {
                        if (state->crt_discard)
                        {
                                if (character == '\n')
                                {
                                        state->crt_discard = false;
                                        state->crt_column = 0;
                                }
                                continue;
                        }

                        if (state->crt_column >= 132)
                        {
                                terminal_crt_output(state, false);
                                state->crt_discard = character != '\n';
                                state->crt_column = 0;
                                continue;
                        }

                        if (character == 033)
                        {
                                p8 command = at < blob->length
                                                 ? blob->bytes[at++]
                                                 : 0;

                                if (command == '8')
                                        terminal_crt_rub(state, 1);
                                else if (command == '7')
                                        terminal_crt_rub(state, 2);
                                else
                                        state->crt_column++;
                                continue;
                        }

                        if (character == '\n')
                        {
                                terminal_crt_output(state, false);
                                state->crt_column = 0;
                                continue;
                        }

                        if (character == '\t')
                        {
                                positive next =
                                    (state->crt_column + 7) & ~(positive)7;

                                while (state->crt_column < next &&
                                       state->crt_column < 132)
                                        text_line[state->crt_column++] = ' ';
                                continue;
                        }

                        if (character == '_')
                        {
                                text_line[state->crt_column] = ' ';
                                if (!state->crt_no_underlining)
                                {
                                        state->crt_need_under = true;
                                        text_record_hold[state->crt_column] = '-';
                                }
                                state->crt_column++;
                                continue;
                        }

                        if (!byte_is_printable(character))
                                continue;

                        state->crt_print_newline = true;
                        text_line[state->crt_column++] = character;
                        continue;
                }

                /* col's cursor protocol is in half-line units. */
                if (character == '\b')
                {
                        if (state->column)
                        {
                                if (state->have_event &&
                                    state->recent_width == 255)
                                        state->column++;
                                else
                                        state->column -=
                                            state->have_event &&
                                                    state->recent_width
                                                ? state->recent_width
                                                : 1;
                        }
                        continue;
                }
                if (character == '\r')
                {
                        state->column = 0;
                        continue;
                }
                if (character == '\n')
                {
                        state->line += 2;
                        if (state->line > state->maximum_line)
                                state->maximum_line = state->line;
                        state->column = 0;
                        continue;
                }
                if (character == '\v')
                {
                        state->line -= 2;
                        continue;
                }
                if (character == '\t')
                {
                        state->column =
                            (state->column + 8) & ~(positive)7;
                        continue;
                }
                if (character == ' ')
                {
                        state->column++;
                        continue;
                }
                if (character == 016)
                {
                        state->character_set = 1;
                        continue;
                }
                if (character == 017)
                {
                        state->character_set = 0;
                        continue;
                }
                if (character == 033)
                {
                        p8 command = at < blob->length ? blob->bytes[at++] : 0;

                        if (command == 007)
                                state->line -= 2;
                        else if (command == '\b')
                                state->line--;
                        else if (command == '\t')
                        {
                                state->line++;
                                if (state->line > state->maximum_line)
                                        state->maximum_line = state->line;
                        }
                        continue;
                }

                if (byte_is_graphic(character))
                        terminal_col_event(state, character, 1, fill);
                else if (state->pass)
                        terminal_col_event(state, character, 255, fill);
        }

        if (state->mode == TERMINAL_UL && !state->ul_failed &&
            state->ul_max_column)
                terminal_ul_flush(state);
}

static fn terminal_half_gap(positive halves, bool fine)
{
        bool half = false;

        if (halves & 1)
        {
                if (fine)
                        half = true;
                else
                        halves++;
        }

        positive lines = halves / 2;
        text_tab_repeat_character('\n', lines);

        if (half)
        {
                text_put_character(033);
                text_put_character('9');
                if (!lines)
                        text_put_character('\r');
        }
}

static fn terminal_col_line(positive first, positive finish,
                            terminal_state address_to state,
                            p8 address_to last_set)
{
        positive last_column = 0;
        positive at = first;

        while (at < finish)
        {
                terminal_event address_to event = terminal_events +
                    (terminal_order ? terminal_order[at] : at);
                positive stop = at + 1;

                while (stop < finish)
                {
                        terminal_event address_to next = terminal_events +
                            (terminal_order ? terminal_order[stop] : stop);
                        if (next->column != event->column)
                                break;
                        stop++;
                }

                positive chosen = state->no_backspaces ? stop - 1 : at;
                terminal_event address_to shown = terminal_events +
                    (terminal_order ? terminal_order[chosen] : chosen);

                if (last_column < shown->column)
                {
                        positive spaces = shown->column - last_column;

                        if (state->compress && spaces > 1)
                        {
                                positive tabs = shown->column / 8 -
                                                last_column / 8;
                                if (tabs)
                                {
                                        text_tab_repeat_character('\t', tabs);
                                        spaces = shown->column & 7;
                                }
                        }

                        text_tab_repeat_character(' ', spaces);
                        last_column = shown->column;
                }

                for (positive one = chosen;
                     one < (state->no_backspaces ? chosen + 1 : stop); one++)
                {
                        shown = terminal_events +
                            (terminal_order ? terminal_order[one] : one);

                        if (shown->set != address_to last_set)
                        {
                                text_put_character(shown->set ? 016 : 017);
                                address_to last_set = shown->set;
                        }

                        text_put_character(shown->character);

                        if (!state->no_backspaces && one + 1 < stop &&
                            shown->width && shown->width != 255)
                                text_tab_repeat_character('\b', shown->width);
                }

                if (shown->width == 255)
                        last_column = shown->column
                                          ? shown->column - 1
                                          : positive_max;
                else if (shown->width)
                        last_column = shown->column + shown->width;

                at = stop;
        }
}

static fn terminal_col_output(terminal_state address_to state)
{
        /* Historical col discards even buffered glyphs when the cursor has
           returned to the origin without ever moving below the first line. */
        if (!state->maximum_line && !state->column)
                return;

        if (!state->events)
        {
                bipolar blank = state->maximum_line;

                if (blank & 1)
                        blank++;
                else if (!blank)
                        blank = 2;

                if (blank > 0)
                        terminal_half_gap((positive)blank, state->fine);
                return;
        }

        terminal_order = null;

        if (!state->ordered && state->events > 1)
        {
                terminal_order = (positive address_to)text_arena_take(
                    state->events * sizeof(positive));
                positive address_to spare =
                    (positive address_to)text_arena_take(
                        state->events * sizeof(positive));

                if (!terminal_order || !spare)
                        return;

                for (positive one = 0; one < state->events; one++)
                        terminal_order[one] = one;

                terminal_order = array_merge_sort(
                    terminal_order, spare, state->events,
                    terminal_event_compare);
        }

        bipolar base = state->minimum_line < 0 ? state->minimum_line : 0;
        bipolar previous = base;
        positive at = 0;
        p8 last_set = 0;

        while (at < state->events)
        {
                terminal_event address_to first = terminal_events +
                    (terminal_order ? terminal_order[at] : at);
                positive finish = at + 1;

                while (finish < state->events &&
                       terminal_events[terminal_order
                                           ? terminal_order[finish]
                                           : finish].line == first->line)
                        finish++;

                if (first->line > previous)
                        terminal_half_gap((positive)(first->line - previous),
                                          state->fine);

                terminal_col_line(at, finish, state, address_of last_set);
                previous = first->line;
                at = finish;
        }

        if (last_set)
                text_put_character(017);

        bipolar allocated = previous > 0 ? previous : 0;
        bipolar tail = state->maximum_line - allocated;

        if (state->maximum_line & 1)
                tail++;
        else if (!tail)
                tail = 2;

        if (tail > 0)
                terminal_half_gap((positive)tail, state->fine);
}

static const file_long col_longs[] = {
    {(string_address)"no-backspaces", 'b'},
    {(string_address)"fine", 'f'},
    {(string_address)"pass", 'p'},
    {(string_address)"tabs", 'h'},
    {(string_address)"spaces", 'x'},
    {(string_address)"lines", 'l'},
    {null, 0},
};

static b32 text_col()
{
        file_taking taking = {
            .program = (string_address)"col",
            .allowed = (string_address)"bfhlpx",
            .valued = (string_address)"l",
            .longs = col_longs,
        };

        text_begin("col");
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.first != (positive)program_argument_count())
                return text_refuse(program_argument((b32)taking.first),
                                   "bad usage", 1);

        if ((taking.flags & FILE_FLAG('h')) &&
            (taking.flags & FILE_FLAG('x')))
                return text_refuse(null,
                                   "--tabs and --spaces are mutually exclusive",
                                   1);

        if (taking.flags & FILE_FLAG('l'))
        {
                positive lines;
                if (!text_unsigned_option(file_option_value(address_of taking,
                                                            'l'),
                                          false, address_of lines) ||
                    lines > 0xffffffffU)
                        return text_refuse(file_option_value(address_of taking,
                                                            'l'),
                                           "bad -l argument", 1);
        }

        text_blob input = {null, 0};
        if (!text_blob_read(null, address_of input))
                return text_done(1);

        terminal_state state = {
            .mode = TERMINAL_COL,
            .fine = (taking.flags & FILE_FLAG('f')) != 0,
            .pass = (taking.flags & FILE_FLAG('p')) != 0,
            .no_backspaces = (taking.flags & FILE_FLAG('b')) != 0,
            .compress = (taking.flags & FILE_FLAG('x')) == 0,
            .ordered = true,
        };

        terminal_scan(address_of input, address_of state, false);
        positive event_count = state.events;
        terminal_events = (terminal_event address_to)text_arena_take(
            event_count * sizeof(terminal_event));
        if (event_count && !terminal_events)
                return text_done(1);

        state = (terminal_state){
            .mode = TERMINAL_COL,
            .fine = (taking.flags & FILE_FLAG('f')) != 0,
            .pass = (taking.flags & FILE_FLAG('p')) != 0,
            .no_backspaces = (taking.flags & FILE_FLAG('b')) != 0,
            .compress = (taking.flags & FILE_FLAG('x')) == 0,
            .ordered = true,
            .event = terminal_events,
        };
        terminal_scan(address_of input, address_of state, true);
        terminal_col_output(address_of state);
        return text_done(text_status);
}

static bool terminal_colcrt_no_under;

static fn terminal_colcrt_operand(b32 which)
{
        string_address word = program_argument(which);

        if (word[0] == '-' && !word[1])
                terminal_colcrt_no_under = true;
        else
                text_file_add(which);
}

static const file_long colcrt_longs[] = {
    {(string_address)"no-underlining", 'q'},
    {(string_address)"half-lines", '2'},
    {null, 0},
};

static b32 text_colcrt()
{
        file_taking taking = {
            .program = (string_address)"colcrt",
            .allowed = (string_address)"2",
            .longs = colcrt_longs,
            .operand = terminal_colcrt_operand,
        };

        text_begin("colcrt");
        terminal_colcrt_no_under = false;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        bool no_under = terminal_colcrt_no_under ||
                        (taking.flags & FILE_FLAG('q'));
        bool half = (taking.flags & FILE_FLAG('2')) != 0;
        b32 inputs = text_input_count();

        for (b32 file = 0; file < inputs; file++)
        {
                text_arena_used = 0;
                text_blob input = {null, 0};

                if (!text_blob_read(text_file_name(file), address_of input))
                        return text_done(1);

                terminal_state state = {
                    .mode = TERMINAL_COLCRT,
                    .crt_no_underlining = no_under,
                    .crt_half_lines = half,
                    .crt_print_newline = true,
                };
                terminal_crt_clear();

                if (half)
                        text_put_character('\n');

                terminal_scan(address_of input, address_of state, true);
                terminal_crt_output(address_of state, true);
        }

        return text_done(text_status);
}

static b32 text_colrm()
{
        text_begin("colrm");
        text_arena_used = 0;

        positive count = (positive)program_argument_count();
        positive first = 0;
        positive last = 0;

        if (count > 1 &&
            !text_unsigned_option(program_argument(1), false,
                                  address_of first))
                return text_refuse(program_argument(1),
                                   "invalid first argument", 1);

        if (count > 2 &&
            !text_unsigned_option(program_argument(2), false,
                                  address_of last))
                return text_refuse(program_argument(2),
                                   "invalid second argument", 1);

        text_blob input = {null, 0};
        if (!text_blob_read(null, address_of input))
                return text_done(1);

        terminal_state state = {
            .mode = TERMINAL_COLRM,
            .remove_first = first,
            .remove_last = last,
        };
        terminal_scan(address_of input, address_of state, true);
        return text_done(text_status);
}

static string_address terminal_ul_option;

static bool terminal_ul_option_seen(p8 letter, string_address value)
{
        if (letter == 't' || letter == 'T')
                terminal_ul_option = value;

        return true;
}

static bool terminal_ul_prefix(string_address value, string_address prefix)
{
        if (!value)
                return false;

        while (prefix[0])
        {
                if (value[0] != prefix[0])
                        return false;

                value++;
                prefix++;
        }

        return true;
}

static p8 terminal_ul_type(string_address name, bool explicit)
{
        if (!name)
        {
                text_error(null, "trouble reading terminfo");
                return TERMINAL_UL_DUMB;
        }

        if (!string_compare(name, (string_address)"dumb"))
                return TERMINAL_UL_DUMB;
        if (!string_compare(name, (string_address)"ansi"))
                return TERMINAL_UL_ANSI;
        if (!string_compare(name, (string_address)"linux"))
                return TERMINAL_UL_LINUX;
        if (!string_compare(name, (string_address)"vt100"))
                return TERMINAL_UL_VT100;
        if (terminal_ul_prefix(name, (string_address)"xterm") ||
            terminal_ul_prefix(name, (string_address)"screen") ||
            terminal_ul_prefix(name, (string_address)"tmux") ||
            terminal_ul_prefix(name, (string_address)"rxvt"))
                return TERMINAL_UL_XTERM;

        if (explicit)
        {
                text_flush();
                text_error_raw("ul: terminal `");
                text_error_raw(name);
                text_error_raw("' is not known, defaulting to `dumb'\n");
        }

        return TERMINAL_UL_DUMB;
}

static const file_long ul_longs[] = {
    {(string_address)"indicated", 'i'},
    {(string_address)"terminal", 't'},
    {null, 0},
};

static b32 text_ul()
{
        file_taking taking = {
            .program = (string_address)"ul",
            .allowed = (string_address)"itT",
            .valued = (string_address)"tT",
            .longs = ul_longs,
            .operand = text_file_add,
            .seen = terminal_ul_option_seen,
        };

        text_begin("ul");
        terminal_ul_option = null;

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        string_address terminal = terminal_ul_option
                                      ? terminal_ul_option
                                      : file_environment(
                                            (string_address)"TERM");
        terminal_state state = {
            .mode = TERMINAL_UL,
            .ul_terminal = terminal_ul_type(terminal,
                                            terminal_ul_option != null),
            .ul_indicated =
                (taking.flags & FILE_FLAG('i')) != 0,
        };
        b32 inputs = text_input_count();

        for (b32 file = 0; file < inputs && !state.ul_failed; file++)
        {
                text_arena_used = 0;
                text_blob input = {null, 0};

                if (!text_blob_read(text_file_name(file), address_of input))
                        return text_done(1);

                terminal_scan(address_of input, address_of state, true);
        }

        return text_done((text_status || state.ul_failed) ? 1 : 0);
}

/*
        Sorted prefix lookup.

        A named regular file is mapped read-only and narrowed to one line by
        byte offsets before any output is touched. A dictionary with millions
        of entries therefore costs logarithmically many page faults, not a
        read of every entry before the one requested. Pipes and pseudo files
        stay on text_line_next, so there is still only one refill path and
        refill-crossing records use its existing spill.

        Exhausting the key is equality: look asks for a prefix, not a complete
        record. The unfiltered paths use the shared wide byte comparators.
        Dictionary order alone needs a byte iterator to omit punctuation
        without manufacturing a transformed copy of every line.
*/
static bool look_dictionary;
static bool look_fold_case;
static p8 address_to look_key;
static positive look_key_length;

static inline INLINE p8 look_case(p8 character)
{
        return look_fold_case && character >= 'A' && character <= 'Z'
                   ? (p8)(character + ('a' - 'A'))
                   : character;
}

static bipolar look_compare(p8 address_to line, positive length)
{
        if (!look_dictionary)
        {
                positive shared = min(length, look_key_length);
                bipolar order = look_fold_case
                                      ? memory_compare_ascii_case(
                                            line, look_key, shared)
                                      : memory_compare(line, look_key, shared);

                if (order)
                        return order < 0 ? -1 : 1;

                return length < look_key_length ? -1 : 0;
        }

        positive at = 0;

        for (positive key_at = 0; key_at < look_key_length; key_at++)
        {
                p8 character;

                do
                {
                        if (at >= length)
                                return -1;

                        character = line[at++];
                } while (!byte_is_alnum(character) && character != ' ' &&
                         character != '\t');

                character = look_case(character);

                if (character != look_key[key_at])
                        return character < look_key[key_at] ? -1 : 1;
        }

        return 0;
}

static positive look_after_line(p8 address_to bytes, positive length,
                                positive at)
{
        p8 address_to newline =
            memory_first_of(bytes + at, '\n', length - at);

        return newline ? (positive)(newline - bytes) + 1 : length;
}

/* The BSD algorithm intentionally returns a line at or before the lower
   bound. Starting there makes the final linear correction robust when a
   very long line straddles every midpoint. */
static positive look_lower_line(p8 address_to bytes, positive length)
{
        positive front = 0;
        positive back = length;
        positive probe = look_after_line(bytes, length,
                                         front + (back - front) / 2);

        while (probe < back && back > front)
        {
                positive finish = look_after_line(bytes, length, probe);
                positive record = finish - probe -
                                  (finish > probe && bytes[finish - 1] == '\n');

                if (look_compare(bytes + probe, record) < 0)
                        front = probe;
                else
                        back = probe;

                probe = look_after_line(bytes, length,
                                        front + (back - front) / 2);
        }

        return front;
}

static bool look_mapped(p8 address_to bytes, positive length)
{
        positive at = look_lower_line(bytes, length);

        while (at < length)
        {
                positive after = look_after_line(bytes, length, at);
                positive record = after - at -
                                  (after > at && bytes[after - 1] == '\n');
                bipolar order = look_compare(bytes + at, record);

                if (order < 0)
                {
                        at = after;
                        continue;
                }
                if (order > 0)
                        return false;

                positive first = at;

                do
                {
                        at = after;

                        if (at >= length)
                                break;

                        after = look_after_line(bytes, length, at);
                        record = after - at -
                                 (after > at && bytes[after - 1] == '\n');
                } while (!look_compare(bytes + at, record));

                text_put(bytes + first, at - first);
                return true;
        }

        return false;
}

static bool look_streamed()
{
        bool found = false;

        while (text_line_next())
        {
                bipolar order = look_compare(text_line, text_line_length);

                if (order < 0)
                        continue;
                if (order > 0)
                        break;

                found = true;
                text_put(text_line, text_line_length);

                if (text_line_ended)
                        text_put_character('\n');
        }

        return found;
}

static const file_long look_longs[] = {
    {(string_address)"alternative", 'a'},
    {(string_address)"binary", 'b'},
    {(string_address)"alphanum", 'd'},
    {(string_address)"ignore-case", 'f'},
    {(string_address)"terminate", 't'},
    {null, 0},
};

static b32 text_look()
{
        file_taking taking = {
            .program = (string_address)"look",
            .allowed = (string_address)"abdft",
            .valued = (string_address)"t",
            .longs = look_longs,
        };

        text_begin("look");
        text_arena_used = 0;
        text_delimiter = '\n';

        if (!file_take(address_of taking))
                return text_done(1);

        positive operands = (positive)program_argument_count() - taking.first;

        if (operands < 1 || operands > 2)
                return text_refuse(null, "bad usage", 1);

        bool supplied = operands == 2;
        string_address key = program_argument((b32)taking.first);
        positive key_length = string_length(key);
        string_address terminate = file_option_value(address_of taking, 't');

        if ((taking.flags & FILE_FLAG('t')) && terminate[0])
        {
                p8 address_to stop = memory_first_of(
                    key, terminate[0], key_length);

                if (stop)
                        key_length = (positive)(stop - key) + 1;
        }

        /* As in util-linux, any implicit dictionary enables -d and -f;
           explicitly naming even the standard dictionary does not. */
        look_dictionary = !supplied ||
                          (taking.flags & FILE_FLAG('d')) != 0;
        look_fold_case = !supplied ||
                         (taking.flags & FILE_FLAG('f')) != 0;
        look_key = (p8 address_to)text_arena_take(key_length + 1);

        if (!look_key)
                return text_done(1);

        look_key_length = 0;

        for (positive i = 0; i < key_length; i++)
        {
                p8 character = key[i];

                if (look_dictionary && !byte_is_alnum(character) &&
                    character != ' ' && character != '\t')
                        continue;

                look_key[look_key_length++] = look_case(character);
        }

        look_key[look_key_length] = end;

        string_address path = null;
        bool already_open = false;

        if (supplied)
                path = program_argument((b32)taking.first + 1);
        else
        {
                if (!(taking.flags & FILE_FLAG('a')))
                {
                        string_address wordlist =
                            file_environment((string_address)"WORDLIST");

                        if (wordlist && wordlist[0])
                        {
                                text_quiet_open = true;
                                already_open = text_reader_open(
                                    address_of text_input, wordlist);
                                text_quiet_open = false;

                                if (already_open)
                                        path = wordlist;
                        }
                }

                if (!path)
                        path = (taking.flags & FILE_FLAG('a'))
                                   ? (string_address)"/usr/share/dict/web2"
                                   : (string_address)"/usr/share/dict/words";
        }

        if (!already_open && !text_open(path))
                return text_done(1);

        bool found = false;
        positive size = 0;

        if (text_input.opened && text_regular_size(text_input.handle,
                                                  address_of size))
        {
                if (size)
                {
                        bipolar mapped = system_call_6(
                            syscall(mmap), 0, size, FILE_PROTECT_READ,
                            FILE_MAP_PRIVATE, text_input.handle, 0);

                        if ((positive)mapped < (positive)-4095)
                        {
                                found = look_mapped((p8 address_to)mapped,
                                                    size);
                                memory_free((address_any)mapped, size);
                                text_close();
                                return text_done(found ? 0 : 1);
                        }
                }
                else
                {
                        text_close();
                        return text_done(1);
                }
        }

        found = look_streamed();
        text_close();
        return text_done(text_status ? 1 : found ? 0 : 1);
}

/* line never needs to retain a record. Emitting each refill span directly
   removes both the 1 MiB line ceiling and a copy while still sharing the
   ordinary text_fill/text_put path. No byte after the first newline is
   consumed. */
static b32 text_line_command()
{
        file_taking taking = {
            .program = (string_address)"line",
            .allowed = (string_address)"",
        };

        text_begin("line");
        text_delimiter = '\n';

        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.first != (positive)program_argument_count())
                return text_refuse(program_argument((b32)taking.first),
                                   "bad usage", 1);

        if (!text_open(null))
                return text_done(1);

        /* On a seekable input, a wide refill is put back before returning.
           A pipe cannot be put back, so match util-linux's unbuffered reader
           there and request exactly one byte. */
        bool seekable = system_seek(text_input.handle, 0, FILE_SEEK_CUR) >= 0;

        while (text_fill_amount(seekable ? TEXT_READ_MAX : 1))
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;
                p8 address_to newline = memory_first_of(at, '\n', left);
                positive take = newline ? (positive)(newline - at) + 1 : left;

                text_put(at, take);
                text_input.position += take;

                if (newline)
                {
                        positive unread = text_input.filled -
                                          text_input.position;

                        if (seekable && unread)
                                system_seek(text_input.handle,
                                            (positive)(-(bipolar)unread),
                                            FILE_SEEK_CUR);

                        text_close();
                        return text_done(0);
                }
        }

        text_put_character('\n');
        text_close();
        return text_done(1);
}

static b32 text_fold()
{
        file_taking taking = {
            .program = (string_address) "fold",
            .allowed = (string_address) "bcsw",
            .valued = (string_address) "w",
            .longs = fold_longs,
            .operand = text_file_add,
            // fold -5 is fold -w 5, and the digits are the width.
            .digits = 'w',
        };

        text_begin("fold");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive width = 80;
        bool spaces = (taking.flags & FILE_FLAG('s')) != 0;
        // -c counts characters but retains the terminal-column rules for
        // tabs, backspaces and carriage returns. Only -b makes each byte a
        // column; treating -c as its alias was observably wrong for controls.
        bool bytes = (taking.flags & FILE_FLAG('b')) != 0;

        if ((taking.flags & FILE_FLAG('w')) &&
            (!text_unsigned_option(file_option_value(address_of taking, 'w'), false,
                                   address_of width) ||
             !width || width == (positive)-1))
                return text_refuse(file_option_value(address_of taking, 'w'),
                                   "invalid number of columns", 1);

        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
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

                                        if (spaces && byte_is_blank(character))
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
/*
        A line that can be accepted can name any of its bytes or fields. The
        old 4096-entry bitmap made `cut -c5000` quietly print an empty line
        from a 6000-byte record. One slot beyond the record is useful for the
        final empty field after a delimiter; anything higher cannot select a
        value before the line reader's explicit ceiling.
*/
#define TEXT_LIST_MAX (TEXT_LINE_MAX + 2)

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
                positive taken;
                positive first = string_digits(spec + at, address_of taken);
                bool have_first = taken != 0;

                at += taken;

                positive last = 0;
                bool have_last = false;

                if (spec[at] == '-')
                {
                        at++;

                        last = string_digits(spec + at, address_of taken);
                        have_last = taken != 0;
                        at += taken;

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

                if (first < TEXT_LIST_MAX)
                {
                        if (!text_list[first])
                                text_list_begins[first] = 1;

                        if (last >= first)
                        {
                                positive through = min(last, TEXT_LIST_MAX - 1);

                                memory_fill(text_list + first, 1, through - first + 1);
                        }
                }

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
static const file_long cut_longs[] = {
    {(string_address) "bytes", 'b'},
    {(string_address) "characters", 'c'},
    {(string_address) "delimiter", 'd'},
    {(string_address) "fields", 'f'},
    {(string_address) "complement", 'C'},
    {(string_address) "no-partial", 'n'},
    {(string_address) "only-delimited", 's'},
    // -O is not in cut's own help and cut takes it anyway, which is where
    // this one comes from.
    {(string_address) "output-delimiter", 'O'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

// A list given twice is two lists, and GNU refuses two -- which one value per
// letter cannot say on its own, so the options are counted as they arrive.
static b32 cut_lists;

static bool cut_list_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'b' || letter == 'c' || letter == 'f')
                cut_lists++;

        return true;
}

static b32 text_cut()
{
        file_taking taking = {
            .program = (string_address) "cut",
            // -n says a multibyte character is not to be split by -b, and
            // every character here is one byte.
            .allowed = (string_address) "Obcdfnswz",
            .valued = (string_address) "Obcdf",
            .longs = cut_longs,
            .operand = text_file_add,
            .seen = cut_list_seen,
        };

        text_begin("cut");

        cut_lists = 0;

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive flags = taking.flags;
        p8 delimiter = '\t';
        bool by_field = (flags & FILE_FLAG('f')) != 0;
        bool by_character = (flags & (FILE_FLAG('b') | FILE_FLAG('c'))) != 0;
        bool only_delimited = (flags & FILE_FLAG('s')) != 0;
        bool have_list = by_field || by_character;
        bool complement = (flags & FILE_FLAG('C')) != 0;
        bool whitespace = (flags & FILE_FLAG('w')) != 0;
        bool have_delimiter = (flags & FILE_FLAG('d')) != 0;
        b32 kinds = cut_lists;
        string_address separator = file_option_value(address_of taking, 'O');
        positive separator_length = separator ? string_length(separator) : 0;
        string_address said = file_option_value(address_of taking,
                                                by_field ? 'f'
                                                : (flags & FILE_FLAG('b')) ? 'b'
                                                                           : 'c');

        if (have_delimiter)
                delimiter = file_option_value(address_of taking, 'd')[0];

        if (flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        if (have_list && kinds == 1 && !text_list_parse(said))
                return text_refuse(null, "invalid list", 1);

        if (!have_list)
                return text_refuse(
                    null,
                    "you must specify a list of bytes, characters, or fields",
                    1);

        /*
                Three ways of saying the same no. GNU refuses two lists of
                different kinds, a delimiter for something that has no fields
                in it, and -s for the same reason -- and refusing them is what
                stops cut -d: -c1 from quietly ignoring the -d.
        */
        if (kinds > 1)
                return text_refuse(null,
                                   "only one type of list may be specified", 1);

        if (whitespace && have_delimiter)
                return text_refuse(null, "-d and -w are mutually exclusive", 1);

        if (!by_field && (have_delimiter || only_delimited || whitespace))
                return text_refuse(
                    null,
                    "an input delimiter makes sense only when operating on fields",
                    1);

        // -w splits on runs of blanks and joins with a tab, which is the one
        // place cut's two delimiters are not the same character.
        if (whitespace && !separator)
        {
                separator = "\t";
                separator_length = 1;
        }

        b32 inputs = text_input_count();

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
                        continue;

                for (;;)
                {
                        p8 address_to line = null;
                        positive line_length = 0;

                        if (!text_line_view(address_of line,
                                            address_of line_length,
                                            null, 0, null))
                                break;

                        if (by_character)
                        {
                                bool wrote = false;
                                bool ran = false;

                                for (positive c = 0; c < line_length; c++)
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

                                        text_put_character(line[c]);
                                        wrote = true;
                                        ran = true;
                                }

                                text_put_character(text_delimiter);
                                continue;
                        }

                        if (by_field)
                        {
                                /*
                                        Without a custom output delimiter the
                                        result cannot be wider than the input
                                        plus its record terminator. Reserve
                                        that maximum once, emit selected
                                        fields directly into it, then release
                                        the unused tail. Learning whether the
                                        record had a delimiter in the same
                                        pass avoids a separate pre-scan.
                                */
                                if (!whitespace && !separator &&
                                    line_length + 1 <= TEXT_OUT_MAX)
                                {
                                        positive reserved = line_length + 1;
                                        p8 address_to out = text_reserve(reserved);

                                        if (!out)
                                                continue;

                                        positive out_length = 0;
                                        positive at = 0;
                                        positive which = 1;
                                        bool wrote = false;
                                        bool split = false;

                                        while (at <= line_length)
                                        {
                                                positive from = at;
                                                p8 address_to next =
                                                    (p8 address_to)memory_first_of(
                                                        line + at, (b8)delimiter,
                                                        line_length - at);

                                                at = next ? (positive)(next - line)
                                                          : line_length;

                                                if (text_list_has(which) != complement)
                                                {
                                                        if (wrote)
                                                                out[out_length++] = delimiter;

                                                        positive take = at - from;
                                                        memory_copy(out + out_length,
                                                                    line + from, take);
                                                        out_length += take;
                                                        wrote = true;
                                                }

                                                if (at == line_length)
                                                        break;

                                                split = true;
                                                at++;
                                                which++;
                                        }

                                        if (!split)
                                        {
                                                if (only_delimited)
                                                        out_length = 0;
                                                else
                                                {
                                                        memory_copy(out, line,
                                                                    line_length);
                                                        out_length = line_length;
                                                        out[out_length++] =
                                                            text_delimiter;
                                                }
                                        }
                                        else
                                                out[out_length++] = text_delimiter;

                                        text_out_used -= reserved - out_length;
                                        continue;
                                }

                                bool split = false;

                                for (positive c = 0; c < line_length; c++)
                                        if (whitespace ? byte_is_blank(line[c])
                                                       : line[c] == delimiter)
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
                                                text_put(line, line_length);
                                                text_put_character(text_delimiter);
                                        }

                                        continue;
                                }

                                positive at = 0;
                                positive which = 1;
                                bool wrote = false;

                                while (at <= line_length)
                                {
                                        positive from = at;

                                        if (whitespace)
                                                at += string_span_max(
                                                    line + at, line_length - at,
                                                    text_inside());
                                        else
                                        {
                                                p8 address_to next =
                                                    (p8 address_to)memory_first_of(
                                                        line + at, (b8)delimiter,
                                                        line_length - at);

                                                at = next ? (positive)(next - line)
                                                          : line_length;
                                        }

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

                                                text_put(line + from, at - from);
                                                wrote = true;
                                        }

                                        if (at == line_length)
                                                break;

                                        // A run of blanks is one delimiter,
                                        // where a run of colons is several.
                                        at++;

                                        if (whitespace)
                                                while (at < line_length &&
                                                       byte_is_blank(line[at]))
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
static bool text_set_broken;

static fn text_set_put(p8 address_to into, positive address_to have, p8 character)
{
        if (address_to have < TEXT_SET_MAX)
        {
                into[address_to have] = character;
                address_to have += 1;
        }
        else
                text_set_broken = true;
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

        p8 escaped = byte_simple_escape(next);

        if (escaped)
                return escaped;

        if (next >= '0' && next <= '7')
        {
                positive used;
                positive value = string_digits_octal_escape_max(
                    spec + address_to at - 1, 3, address_of used);

                address_to at += used - 1;

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
                        positive used;
                        b32 class = byte_class_parse(spec + at, length - at,
                                                     address_of used);

                        if (class >= 0)
                        {
                                for (b32 c = 0; c < 256; c++)
                                        if (byte_class_holds(class, (p8)c))
                                                text_set_put(into, have, (p8)c);
                                at += used;
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

                        while (scan < length && byte_is_digit(spec[scan]))
                        {
                                positive digit = (positive)(spec[scan] - '0');

                                if (count > (positive_max - digit) / base)
                                        text_set_broken = true;
                                else
                                        count = count * base + digit;

                                digits = true;
                                scan++;
                        }

                        if (scan < length && spec[scan] == ']')
                        {
                                if (!digits)
                                        count = TEXT_SET_MAX - address_to have;

                                positive room = TEXT_SET_MAX - address_to have;

                                if (count > room)
                                {
                                        if (digits)
                                                text_set_broken = true;

                                        count = room;
                                }

                                if (count)
                                        memory_fill(into + address_to have,
                                                    repeated, count);

                                address_to have += count;

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

}

static const file_long tr_longs[] = {
    {(string_address) "complement", 'c'},
    {(string_address) "delete", 'd'},
    {(string_address) "squeeze-repeats", 's'},
    {(string_address) "truncate-set1", 't'},
    {null, 0},
};

static b32 text_tr()
{
        file_taking taking = {
            .program = (string_address) "tr",
            .allowed = (string_address) "Ccdst",
            .valued = (string_address) "",
            .longs = tr_longs,
        };

        text_begin("tr");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive flags = taking.flags;
        bool remove = (flags & FILE_FLAG('d')) != 0;
        bool squeeze = (flags & FILE_FLAG('s')) != 0;
        bool complement = (flags & (FILE_FLAG('c') | FILE_FLAG('C'))) != 0;
        bool truncate = (flags & FILE_FLAG('t')) != 0;
        b32 at = (b32)taking.first;
        string_address first = at < text_argument_count ? program_argument(at++) : null;
        string_address second = at < text_argument_count ? program_argument(at++) : null;
        string_address extra = at < text_argument_count ? program_argument(at++) : null;

        if (!first)
                return text_refuse(null, "missing operand", 1);

        /*
                How many sets each shape of tr wants, which it has to say out
                loud rather than quietly ignore the ones it did not use. Two
                to translate; one to delete or to squeeze; two to delete and
                squeeze at once, because the second is what gets squeezed.
        */
        if (extra || (remove && !squeeze && second))
                return text_refuse(extra ? extra : second, "extra operand", 1);

        if (!second && !remove && !squeeze)
                return text_refuse(first, "missing operand after", 1);

        text_set_broken = false;
        text_set_build(first, text_set_one, address_of text_set_one_length);

        if (second)
                text_set_build(second, text_set_two, address_of text_set_two_length);

        if (text_set_broken)
                return text_refuse(null, "set too large", 1);

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

        // Without a second set, squeezing looks at the first one. With one,
        // it is the second whether or not the first is being deleted: -ds
        // deletes SET1 and squeezes SET2, and looking at SET1 there squeezed
        // nothing, since every byte of it had just been deleted.
        p8 address_to squeezed = second ? in_second : in_first;

        if (!text_open(null))
                return text_done(1);

        b32 last_written = -1;

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;

                if (!remove && !squeeze)
                {
                        memory_translate(at, left, mapped);
                        text_put(at, left);
                        text_input.position = text_input.filled;
                        continue;
                }

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
        something different either way, which is what `optional` below is for:
        the letter is set either way and only carries a value when one came.
*/
// A name that carries a word needs a letter of its own: -D is a plain flag
// and -Dc is two of them, where --all-repeated=WORD is one option and a word.
// A and G are letters uniq has not got and that `allowed` goes on refusing.
static const file_long uniq_longs[] = {
    {(string_address) "count", 'c'},
    {(string_address) "repeated", 'd'},
    {(string_address) "all-repeated", 'A'},
    {(string_address) "group", 'G'},
    {(string_address) "skip-fields", 'f'},
    {(string_address) "ignore-case", 'i'},
    {(string_address) "skip-chars", 's'},
    {(string_address) "unique", 'u'},
    {(string_address) "check-chars", 'w'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

enum
{
        UNIQ_GROUP_NONE = 0,
        UNIQ_GROUP_SEPARATE,
        UNIQ_GROUP_PREPEND,
        UNIQ_GROUP_APPEND,
        UNIQ_GROUP_BOTH
};

// The words both names take, and the two more that only --group does.
static bool uniq_grouping_of(string_address word, bool ends, positive address_to how)
{
        if (string_equals(word, "none"))
                address_to how = UNIQ_GROUP_NONE;
        else if (string_equals(word, "prepend"))
                address_to how = UNIQ_GROUP_PREPEND;
        else if (string_equals(word, "separate"))
                address_to how = UNIQ_GROUP_SEPARATE;
        else if (ends && string_equals(word, "append"))
                address_to how = UNIQ_GROUP_APPEND;
        else if (ends && string_equals(word, "both"))
                address_to how = UNIQ_GROUP_BOTH;
        else
                return false;

        return true;
}

static bool uniq_number_of(file_taking address_to taking, p8 letter,
                           positive address_to into)
{
        if (!(taking->flags & FILE_FLAG(letter)))
                return true;

        if (string_digits_exact(file_option_value(taking, letter), into))
                return true;

        text_error(null, "invalid number");
        return false;
}

/*
        Where the compared part of a line begins.

        -f skips whole fields and -s skips characters after them, in that
        order, which is the order POSIX puts them in. A field is the blanks
        leading it together with the run that follows, so skipping one takes
        two spans and not one. A skip past the end of the line leaves nothing
        to compare rather than reading past it.

        uniq asks this of the line it just read and again of the line before
        it, which is why it is here and not written out twice inside the
        loop.
*/
static positive uniq_skipped(p8 address_to line, positive length,
                             positive fields, positive characters)
{
        positive skip = 0;

        for (positive f = 0; f < fields; f++)
        {
                skip += string_span_max(line + skip, length - skip,
                                        string_set_blanks);
                skip += string_span_max(line + skip, length - skip,
                                        text_inside());
        }

        skip += characters;

        return skip > length ? length : skip;
}

static b32 text_uniq()
{
        file_taking taking = {
            .program = (string_address) "uniq",
            .allowed = (string_address) "Dcdfisuwz",
            .valued = (string_address) "fsw",
            .optional = (string_address) "AG",
            .longs = uniq_longs,
            .operand = text_file_add,
        };

        text_begin("uniq");

        if (!file_take(address_of taking))
                return text_done(1);

        if (!text_files_ready())
                return text_done(1);

        positive flags = taking.flags;
        bool counting = (flags & FILE_FLAG('c')) != 0;
        bool repeated_only = (flags & FILE_FLAG('d')) != 0;
        bool unique_only = (flags & FILE_FLAG('u')) != 0;
        bool fold = (flags & FILE_FLAG('i')) != 0;
        bool all_repeated = (flags & (FILE_FLAG('D') | FILE_FLAG('A'))) != 0;
        bool grouping = (flags & FILE_FLAG('G')) != 0;
        bool bounded = (flags & FILE_FLAG('w')) != 0;
        positive all_how = UNIQ_GROUP_NONE;
        positive group_how = UNIQ_GROUP_SEPARATE;
        positive skip_fields = 0;
        positive skip_characters = 0;
        positive compare_width = 0;
        string_address said = file_option_value(address_of taking, 'A');

        if (flags & FILE_FLAG('z'))
                text_delimiter = '\0';

        if (said && !uniq_grouping_of(said, false, address_of all_how))
                return text_refuse(said, "invalid argument", 1);

        said = file_option_value(address_of taking, 'G');

        if (said && !uniq_grouping_of(said, true, address_of group_how))
                return text_refuse(said, "invalid argument", 1);

        if (!uniq_number_of(address_of taking, 'f', address_of skip_fields) ||
            !uniq_number_of(address_of taking, 's', address_of skip_characters) ||
            !uniq_number_of(address_of taking, 'w', address_of compare_width))
                return text_done(1);

        if (all_repeated && counting)
                return text_refuse(
                    null,
                    "printing all duplicated lines and repeat counts is meaningless",
                    1);

        if (grouping && (counting || repeated_only || unique_only || all_repeated))
                return text_refuse(
                    null, "--group is mutually exclusive with -c/-d/-D/-u", 1);

        if (text_files_count > 2)
                return text_refuse(program_argument(text_files[2]),
                                   "extra operand", 1);

        if (!text_open(text_file_name(0)))
                return text_done(1);

        // uniq's second operand is where the answer goes, not another input.
        if (text_files_count > 1)
        {
                string_address name = program_argument(text_files[1]);
                bipolar target = text_open_handle(name, TEXT_WRITE, 0666);

                if (target < 0)
                        return text_refuse(name, "Cannot open file", 1);

                text_out_handle = (positive)target;
        }

        // uniq puts a terminator on every line it writes, including the last
        // one when the input did not have one. Every other tool here hands
        // back what it was given; measured, this one does not.
        // Most adjacent records live in the same reader fill. Keep a view of
        // the prior one and copy it here only before a refill can invalidate
        // that view. The spare byte carries its terminator with it.
        p8 address_to previous = text_record_hold;
        positive previous_length = 0;
        bool have_previous = false;
        bool shown_group = false;
        positive count = 0;
        positive gap = grouping ? group_how : all_how;

        for (;;)
        {
                p8 address_to line = null;
                positive line_length = 0;
                bool more = text_line_view(address_of line,
                                           address_of line_length,
                                           address_of previous,
                                           previous_length, text_record_hold);

                if (more)
                {
                        // The compared part starts after the skipped fields
                        // and then after the skipped characters, which is the
                        // order POSIX puts them in.
                        positive skip = uniq_skipped(
                            line, line_length, skip_fields, skip_characters);
                        positive previous_skip = uniq_skipped(
                            previous, previous_length, skip_fields,
                            skip_characters);

                        positive one = line_length - skip;
                        positive two = previous_length - previous_skip;

                        if (bounded)
                        {
                                if (one > compare_width)
                                        one = compare_width;

                                if (two > compare_width)
                                        two = compare_width;
                        }

                        bool same = have_previous && one == two;

                        if (same)
                                same = !(fold
                                             ? memory_compare_ascii_case(
                                                   line + skip,
                                                   previous + previous_skip, one)
                                             : memory_compare(line + skip,
                                                              previous + previous_skip, one));

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
                                        text_put(previous, previous_length + 1);
                                }

                                if ((all_repeated && count >= 2) || grouping)
                                {
                                        text_put(line, line_length + 1);
                                }

                                continue;
                        }
                }

                if (have_previous && !all_repeated && !grouping)
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
                                        positive width = positive_digits(count);

                                        if (width < 7)
                                                width = 7;

                                        positive total = width + 1 +
                                                         previous_length + 1;

                                        if (total <= TEXT_OUT_MAX)
                                        {
                                                p8 address_to out = text_reserve(total);

                                                if (out)
                                                {
                                                        positive length =
                                                            positive_into_padded(
                                                                out, count, 7, ' ');
                                                        out[length++] = ' ';
                                                        memory_copy(out + length,
                                                                    previous,
                                                                    previous_length + 1);
                                                }
                                        }
                                        else
                                        {
                                                // Width is a minimum, never a
                                                // digit cap; the stack field
                                                // therefore needs twenty
                                                // digits plus its separator.
                                                p8 field[21];
                                                positive length =
                                                    positive_into_padded(
                                                        field, count, 7, ' ');
                                                field[length++] = ' ';
                                                text_put(field, length);
                                                text_put(previous,
                                                         previous_length + 1);
                                        }
                                }
                                else
                                        text_put(previous, previous_length + 1);
                        }
                }

                if (!more)
                        break;

                previous = line;
                previous_length = line_length;
                have_previous = true;
                count = 1;

                if (grouping)
                {
                        if (gap == UNIQ_GROUP_PREPEND || gap == UNIQ_GROUP_BOTH ||
                            ((gap == UNIQ_GROUP_SEPARATE || gap == UNIQ_GROUP_APPEND) &&
                             shown_group))
                                text_put_character('\n');

                        shown_group = true;
                        text_put(previous, previous_length + 1);
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
// How many groups the patterns joined so far have opened, which is what a
// backreference in the next one has to be counted past.
static positive grep_pattern_groups;

/*
        The groups a pattern opens, and its backreferences moved along by
        the groups placed in front of it.

        Joining -e patterns with an alternation and wrapping the whole in
        the groups -x and -w need both put groups before the user's own, so
        \1 in the second -e pattern named the first pattern's group and \1
        under -x named the wrapper's. Bracket expressions are stepped over,
        since a backslash and a digit are two bytes in one; a backslash pair
        is one escaped byte and looked at no further.
*/
static positive grep_pattern_set_end(string_address text, positive length,
                                     positive at)
{
        // A ] first thing in the set, after any ^, is a member.
        at++;

        if (at < length && text[at] == '^')
                at++;

        if (at < length && text[at] == ']')
                at++;

        while (at < length)
        {
                string_address past = byte_class_end(text + at, text + length);

                if (past)
                {
                        at = (positive)(past - text);
                        continue;
                }

                if (text[at] == ']')
                        return at + 1;

                at++;
        }

        return length;
}

static positive grep_groups_in(string_address text, positive length,
                               bool extended)
{
        positive groups = 0;

        for (positive at = 0; at < length; at++)
        {
                p8 byte = text[at];

                if (byte == '[')
                {
                        at = grep_pattern_set_end(text, length, at) - 1;
                        continue;
                }

                if (byte == '\\' && at + 1 < length)
                {
                        groups += !extended && text[at + 1] == '(';
                        at++;
                        continue;
                }

                groups += extended && byte == '(';
        }

        return groups;
}

static fn grep_shift_references(p8 address_to text, positive length,
                                positive shift, bool extended)
{
        (void)extended;

        for (positive at = 0; at < length; at++)
        {
                p8 byte = text[at];

                if (byte == '[')
                {
                        at = grep_pattern_set_end(text, length, at) - 1;
                        continue;
                }

                if (byte != '\\' || at + 1 >= length)
                        continue;

                p8 next = text[at + 1];

                if (next >= '1' && next <= '9' && (positive)(next - '0') + shift <= 9)
                        text[at + 1] = (p8)(next + shift);

                at++;
        }
}
static bool grep_pattern_broken;

#define grep_pattern_put(character)                                          \
        fixed_store_byte(grep_pattern, grep_pattern_length,                  \
                         grep_pattern_broken, character)

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

                positive placed = grep_pattern_length;

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

                // A fixed string has no groups and no references; a pattern
                // has its references counted past the groups before it.
                if (!fixed && placed <= grep_pattern_length)
                {
                        grep_shift_references(grep_pattern + placed,
                                              grep_pattern_length - placed,
                                              grep_pattern_groups, extended);
                        grep_pattern_groups += grep_groups_in(
                            grep_pattern + placed, grep_pattern_length - placed,
                            extended);
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
        The oldest line goes when all requested slots are full. Exhausting the
        byte pool is different: evicting then would print less context than
        was requested, so that bounded case is refused aloud.
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
static p8 address_to grep_hold_color;
static bool grep_coloring;
static bool grep_color_ne;
static bool grep_color_reverse;
static string_address grep_colors;
static positive grep_match_slot;

static fn grep_color_line(string_address line, positive length, bool context,
                          bool highlight);

static bool grep_hold_make(positive lines)
{
        if (lines > GREP_HOLD_LINES)
        {
                text_error(null, "context length too large");
                return false;
        }

        grep_hold_slots = lines;
        grep_hold_pool = (p8 address_to)text_arena_take(GREP_HOLD_BYTES);
        grep_hold_at = (positive address_to)text_arena_take(lines * sizeof(positive));
        grep_hold_size = (positive address_to)text_arena_take(lines * sizeof(positive));
        grep_hold_number = (positive address_to)text_arena_take(lines * sizeof(positive));
        grep_hold_color = grep_coloring
                              ? (p8 address_to)text_arena_take(GREP_HOLD_BYTES)
                              : null;

        return grep_hold_pool && grep_hold_at && grep_hold_size &&
               grep_hold_number && (!grep_coloring || grep_hold_color);
}

/*
        The lines that cannot match, stepped over without being read.

        A line with none of the pattern's fixed string in it cannot match the
        pattern, and one wide search says so for a whole block at once instead
        of the reader splitting four hundred thousand lines and the machine
        being asked about each. What is skipped is still counted, because -n
        and -b want to know: memory_count over the same bytes answers both.

        Never past the start of the last whole line in the block, so a match
        that straddles the boundary is left for the reader, which knows how to
        carry a line across a refill and this does not.
*/
static p8 grep_literal[REGEX_LITERAL_MAX];
static positive grep_literal_length;
static bool grep_literal_icase;
static positive2 grep_literal_anchors;

static fn grep_literal_keep()
{
        grep_literal_length = regex_literal_length;
        memory_copy(grep_literal, regex_literal, regex_literal_length);
        grep_literal_anchors = regex_literal_anchors;
}

static bool grep_skip(positive address_to lines, positive address_to bytes)
{
        for (;;)
        {
                if (!text_fill())
                        return false;

                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;
                string_address found = grep_literal_icase
                                           ? memory_search_ascii_case_prepared(
                                                 at, left, grep_literal,
                                                 grep_literal_length,
                                                 grep_literal_anchors.x)
                                           : memory_search_prepared(
                                                 at, left, grep_literal,
                                                 grep_literal_length,
                                                 grep_literal_anchors.x,
                                                 grep_literal_anchors.y);
                positive stop = found ? (positive)(found - at) : left;

                {
                        p8 address_to last = (p8 address_to)memory_last_of(
                            at, (b8)text_delimiter, stop);

                        stop = last ? (positive)(last - at) + 1 : 0;
                }

                if (lines)
                        address_to lines += memory_count(at, stop,
                                                         text_delimiter);
                if (bytes)
                        address_to bytes += stop;
                text_input.position += stop;

                if (found)
                        return true;

                if (text_input.position < text_input.filled)
                        return false;
        }
}

/* Counting a proven literal match does not need the line copied into the
   parser buffer. Consume through its delimiter in place, carrying a final
   unterminated line across refills. */
static bool grep_discard_line()
{
        bool any = false;

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;
                p8 address_to delimiter = memory_first_of(
                    at, text_delimiter, left);

                if (delimiter)
                {
                        text_input.position += (positive)(delimiter - at) + 1;
                        return true;
                }

                any = any || left;
                text_input.position = text_input.filled;
        }

        return any;
}

static fn grep_hold_clear()
{
        grep_hold_first = 0;
        grep_hold_count = 0;
        grep_hold_used = 0;
        grep_hold_write = 0;
}

static bool grep_hold_put(string_address line, positive length, positive number)
{
        if (!grep_hold_slots)
                return true;

        if (length > GREP_HOLD_BYTES)
        {
                text_error(null, "context lines too large");
                return false;
        }

        while (grep_hold_count == grep_hold_slots)
        {
                grep_hold_used -= grep_hold_size[grep_hold_first];
                grep_hold_first = (grep_hold_first + 1) % grep_hold_slots;
                grep_hold_count--;
        }

        if (grep_hold_used + length > GREP_HOLD_BYTES)
        {
                text_error(null, "context lines too large");
                return false;
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
        return true;
}

static fn grep_hold_say(positive slot, bool highlight)
{
        positive at = grep_hold_at[slot];
        positive length = grep_hold_size[slot];
        positive head = GREP_HOLD_BYTES - at;

        if (head > length)
                head = length;

        if (!grep_coloring)
        {
                text_put(grep_hold_pool + at, head);

                if (length > head)
                        text_put(grep_hold_pool, length - head);

                return;
        }

        if (length > head)
        {
                memory_copy(grep_hold_color, grep_hold_pool + at, head);
                memory_copy(grep_hold_color + head, grep_hold_pool, length - head);
                grep_color_line(grep_hold_color, length, true, highlight);
        }
        else
                grep_color_line(grep_hold_pool + at, length, true, highlight);
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
static file_color_span grep_color_value(string_address key,
                                        string_address fallback)
{
        // GREP_COLORS is the current GNU interface. The deprecated singular
        // GREP_COLOR variable is deliberately not another precedence layer.
        bool match_alias = string_equals(key, "ms") || string_equals(key, "mc");

        return file_color_value_aliased(
            grep_colors, key,
            match_alias ? (string_address) "mt" : null, fallback);
}

static fn grep_color_start(file_color_span color)
{
        if (!color.length)
                return;

        file_color_sgr(text_put, color);

        if (!grep_color_ne)
                text_put_string((string_address) "\033[K");
}

static fn grep_color_stop()
{
        text_put_string((string_address) "\033[m");

        if (!grep_color_ne)
                text_put_string((string_address) "\033[K");
}

static fn grep_color_field(address_any data, positive length,
                           string_address key, string_address fallback)
{
        if (!grep_coloring)
        {
                text_put(data, length);
                return;
        }

        file_color_span color = grep_color_value(key, fallback);

        if (!color.length)
        {
                text_put(data, length);
                return;
        }

        grep_color_start(color);
        text_put(data, length);
        grep_color_stop();
}

static fn grep_color_separator(p8 separator)
{
        grep_color_field(address_of separator, 1, (string_address) "se",
                         (string_address) "36");
}

/*
        The "--" that stands between two runs of context lines.

        It goes in when the run about to be printed is not the one that
        followed the last line printed: either something has been skipped
        since -- a new file, a jump the -A and -B windows did not cover -- or
        the line numbers have a gap in them. Two runs that touch get nothing
        between them.

        Saying it also ends the pending gap, so the flag is cleared here
        rather than at each of the three places that print a run.
*/
static fn grep_group_gap(bool grouped, string_address separator,
                         bool address_to split, positive shown, positive number)
{
        if (grouped && separator &&
            (address_to split || (shown && number > shown + 1)))
        {
                grep_color_field(separator, string_length(separator),
                                 (string_address) "se", (string_address) "36");
                text_put_character('\n');
        }

        address_to split = false;
}

static fn grep_head(string_address name, p8 separator, positive number, positive offset)
{
        if (grep_names)
        {
                grep_color_field(name, string_length(name), (string_address) "fn",
                                 (string_address) "35");

                if (grep_null_name)
                        text_put_character('\0');
                else
                        grep_color_separator(separator);
        }

        if (grep_numbered)
        {
                file_color_span color = grep_color_value((string_address) "ln",
                                                         (string_address) "32");

                if (grep_coloring && color.length)
                        grep_color_start(color);

                positive_to_padded(text_put, number, grep_tabbed ? grep_column : 1,
                                   ' ', 0);

                if (grep_coloring && color.length)
                        grep_color_stop();

                grep_color_separator(separator);
        }

        if (grep_offsets)
        {
                file_color_span color = grep_color_value((string_address) "bn",
                                                         (string_address) "32");

                if (grep_coloring && color.length)
                        grep_color_start(color);

                positive_to_padded(text_put, offset, grep_tabbed ? grep_column : 1,
                                   ' ', 0);

                if (grep_coloring && color.length)
                        grep_color_stop();

                grep_color_separator(separator);
        }

        if (grep_tabbed && (grep_names || grep_numbered || grep_offsets))
                text_put_character('\t');
}

static fn grep_color_line(string_address line, positive length, bool context,
                          bool highlight)
{
        if (!grep_coloring)
        {
                text_put(line, length);
                return;
        }

        bool context_style = context != grep_color_reverse;
        file_color_span line_color = grep_color_value(
            context_style ? (string_address) "cx" : (string_address) "sl",
            (string_address) "");
        file_color_span match_color = grep_color_value(
            context ? (string_address) "mc" : (string_address) "ms",
            (string_address) "01;31");
        bool line_styled = line_color.length != 0;
        positive from = 0;
        positive search = 0;

        if (line_styled)
                grep_color_start(line_color);

        while (highlight && search <= length &&
               regex_search_longest(line, length, search))
        {
                positive whole_stop = regex_slots[1];
                positive begin = regex_slots[grep_match_slot];
                positive stop = regex_slots[grep_match_slot + 1];

                if (stop == begin)
                {
                        if (begin >= length)
                                break;

                        search = begin + 1;
                        continue;
                }

                text_put(line + from, begin - from);

                if (match_color.length)
                {
                        grep_color_start(match_color);
                        text_put(line + begin, stop - begin);
                        grep_color_stop();
                }
                else
                        text_put(line + begin, stop - begin);

                if (line_styled)
                        grep_color_start(line_color);

                from = stop;
                // From the end of the word, not of the match: -w's wrapper
                // takes the separator after a word with it, and the word
                // after that separator needs it in front to be a word.
                search = grep_match_slot ? stop : whole_stop;
        }

        text_put(line + from, length - from);

        if (line_styled)
                grep_color_stop();
}

/*
        The globs --include and the two beside it are matched against.

        A file's own name and never the path it was found down: GNU matches
        d/sub/s.txt against s.txt and against nothing with a slash in it, so
        there is no path walking here and * does not have to stop anywhere.

        The matcher is the shell's own, which is where every glob in this tree
        goes now; file.c declares it, being the first of the two read.
*/

/*
        -r turns a directory operand into the files under it, in the order the
        kernel hands them back: nothing here sorts, and neither does GNU's own
        walk, so the two agree line for line on the same directory.

        A symlink met on the way down is skipped, which is what -r means and
        what -R undoes. The one named on the command line is followed either
        way -- opening it is how it was found at all.
*/
#define GREP_PATHS_MAX (1 << 20)

static string_address address_to grep_paths;
static positive grep_path_count;
static positive grep_paths_room;
static bool grep_recursive;
static bool grep_dereference;
static bool grep_expanded;
static bool grep_skip_directories;

typedef struct grep_glob
{
        struct grep_glob address_to next;
        string_address value;
} grep_glob;

static grep_glob address_to grep_include;
static grep_glob address_to grep_exclude;
static grep_glob address_to grep_exclude_dir;

/*
        All three option families are the same list operation. Values from an
        exclude file have to outlive the shared line reader, and copying the
        argv values as well keeps that lifetime rule in one place. The text
        arena makes the number and total width of globs an input limit rather
        than three unrelated 32-entry/4 KiB ceilings.

        --exclude-dir=sub and --exclude-dir=sub/ name the same directory.
*/
static bool grep_glob_add(grep_glob address_to address_to list,
                          string_address value, positive length, bool directory)
{
        if (directory)
                while (length && value[length - 1] == '/')
                        length--;

        grep_glob address_to made = (grep_glob address_to)text_arena_take(
            sizeof(grep_glob) + length + 1);

        if (!made)
                return false;

        p8 address_to room = (p8 address_to)(made + 1);

        memory_copy_apart_end(room, value, length);
        made->value = (string_address)room;
        made->next = *list;
        *list = made;

        return true;
}

static PURE bool grep_globs_have(grep_glob address_to list, string_address name)
{
        for (; list; list = list->next)
                if (shell_match(list->value, name))
                        return true;

        return false;
}

static PURE bool grep_wanted_file(string_address path)
{
        string_address name = file_last_component(path);

        if (grep_include && !grep_globs_have(grep_include, name))
                return false;

        return !grep_globs_have(grep_exclude, name);
}

static PURE bool grep_wanted_directory(string_address path)
{
        return !grep_globs_have(grep_exclude_dir, file_last_component(path));
}

// The kernel's mode for a path, or zero when there is none to be had.
static p32 text_path_mode(string_address path)
{
        file_facts facts;
        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
                return 0;

        bool told = text_handle_facts((positive)handle, address_of facts);

        system_close(handle);

        return told ? facts.mode : 0;
}

static bool grep_path_add(string_address path)
{
        if (grep_path_count >= grep_paths_room)
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

        memory_copy_apart_end(room + have, name, extra);

        return (string_address)room;
}

// What getdents says an entry is, where it says anything: a filesystem that
// does not know answers DIRENT_UNKNOWN and the answer is taken from a stat.
enum
{
        DIRENT_UNKNOWN = 0,
        DIRENT_OTHER = 1,
        DIRENT_DIRECTORY = 4,
        DIRENT_FILE = 8,
        DIRENT_LINK = 10
};

#define GREP_DIRENT_BYTES 2048
// A symlink that points at a directory above it is a walk with no end, and -R
// follows symlinks. The device and node of everything currently being walked
// through stops that where it starts, and the depth stops what the pair
// cannot -- a mount arranged to be its own child.
#define GREP_DEPTH_MAX 64

static positive grep_seen_device[GREP_DEPTH_MAX + 1];
static positive grep_seen_node[GREP_DEPTH_MAX + 1];

static bool grep_walk(string_address path, b32 depth)
{
        bipolar handle;
        p8 entries[GREP_DIRENT_BYTES];
        bool fine = true;

        // Too deep is nothing more down here rather than a failure: GNU says
        // it found a loop and carries on with what is beside it.
        if (depth > GREP_DEPTH_MAX)
                return true;

        handle = text_open_handle(path && path[0] ? path : (string_address) ".",
                                  FILE_READ, 0);

        if (handle < 0)
                return false;

        file_facts facts;

        if (text_handle_facts((positive)handle, address_of facts))
        {
                positive device = file_device_key(facts.device_major,
                                                  facts.device_minor);
                positive node = facts.inode;

                for (b32 up = 0; up < depth; up++)
                        if (grep_seen_device[up] == device &&
                            grep_seen_node[up] == node)
                        {
                                system_close(handle);
                                return true;
                        }

                grep_seen_device[depth] = device;
                grep_seen_node[depth] = node;
        }

        while (fine)
        {
                bipolar got = system_read_directory(handle, entries,
                                                    sizeof(entries));

                if (got <= 0)
                        break;

                for (p8 address_to step = entries; step < entries + got && fine;)
                {
                        struct linux_dirent64 address_to entry =
                            (struct linux_dirent64 address_to)step;
                        string_address name = (string_address)entry->d_name;
                        p8 kind = entry->d_type;

                        step += entry->d_reclen;

                        if (name[0] == '.' &&
                            (!name[1] || (name[1] == '.' && !name[2])))
                                continue;

                        if (kind == DIRENT_LINK && !grep_dereference)
                                continue;

                        string_address full = grep_path_join(path, name);

                        if (!full)
                        {
                                fine = false;
                                break;
                        }

                        if (kind == DIRENT_UNKNOWN || kind == DIRENT_LINK)
                        {
                                p32 mode = text_path_mode(full);

                                if (!mode)
                                        continue;

                                kind = (mode & 0170000) == 0040000 ? DIRENT_DIRECTORY
                                     : (mode & 0170000) == 0100000 ? DIRENT_FILE
                                                                   : DIRENT_OTHER;
                        }

                        if (kind == DIRENT_DIRECTORY)
                        {
                                if (grep_wanted_directory(full) &&
                                    !grep_walk(full, depth + 1))
                                        fine = false;

                                continue;
                        }

                        // Anything that is not a plain file is a device, and a
                        // walk does not read devices.
                        if (kind != DIRENT_FILE)
                                continue;

                        if (grep_wanted_file(full) && !grep_path_add(full))
                                fine = false;
                }
        }

        system_close(handle);
        return fine;
}

/*
        The long spellings grep answers to.

        --label and the ten beside it have no letter of their own, so each
        borrows a letter grep has not got: `allowed` leaves every one of them
        out, and grep -J is still the mistake it always was.

        Not here, and deliberately: -P and --perl-regexp, which is a second
        regular expression language. Colour is handled at the output seams,
        after matching, so it never enters byte offsets or regular expressions.
*/
static const file_long grep_longs[] = {
    {(string_address) "extended-regexp", 'E'},
    {(string_address) "fixed-strings", 'F'},
    {(string_address) "basic-regexp", 'G'},
    {(string_address) "regexp", 'e'},
    {(string_address) "file", 'f'},
    {(string_address) "ignore-case", 'i'},
    {(string_address) "no-ignore-case", 'M'},
    {(string_address) "word-regexp", 'w'},
    {(string_address) "line-regexp", 'x'},
    {(string_address) "no-messages", 's'},
    {(string_address) "invert-match", 'v'},
    {(string_address) "max-count", 'm'},
    {(string_address) "byte-offset", 'b'},
    {(string_address) "line-number", 'n'},
    {(string_address) "line-buffered", 'K'},
    {(string_address) "with-filename", 'H'},
    {(string_address) "no-filename", 'h'},
    {(string_address) "label", 'J'},
    {(string_address) "only-matching", 'o'},
    {(string_address) "quiet", 'q'},
    {(string_address) "silent", 'q'},
    {(string_address) "binary-files", 'N'},
    {(string_address) "text", 'a'},
    {(string_address) "binary", 'U'},
    {(string_address) "files-without-match", 'L'},
    {(string_address) "files-with-matches", 'l'},
    {(string_address) "count", 'c'},
    {(string_address) "initial-tab", 'T'},
    {(string_address) "null", 'Z'},
    {(string_address) "null-data", 'z'},
    {(string_address) "directories", 'd'},
    {(string_address) "devices", 'D'},
    {(string_address) "group-separator", 'O'},
    {(string_address) "no-group-separator", 'P'},
    {(string_address) "before-context", 'B'},
    {(string_address) "after-context", 'A'},
    {(string_address) "context", 'C'},
    {(string_address) "recursive", 'r'},
    {(string_address) "dereference-recursive", 'R'},
    {(string_address) "include", 'Q'},
    {(string_address) "exclude", 'S'},
    {(string_address) "exclude-from", 'X'},
    {(string_address) "exclude-dir", 'V'},
    {(string_address) "color", 'W'},
    {(string_address) "colour", 'W'},
    {null, 0},
};

/*
        What has to be done in the order it was written.

        -e and -f come as often as there are patterns, and the three glob
        options as often as there are globs, so one value per letter is not
        enough to hold them. -E and -F are here for the same reason from the
        other side: a pattern is compiled as the language said when the
        pattern arrived, so grep -e a -E and grep -E -e a are not the same.
*/
static bool grep_fixed;
static bool grep_extended;
static bool grep_icase;
static bool grep_never;
static bool grep_said_pattern;
static b32 grep_pattern_from;

static fn grep_operand(b32 index)
{
        if (!grep_said_pattern && grep_pattern_from < 0)
        {
                grep_pattern_from = index;
                grep_said_pattern = true;
                return;
        }

        text_file_add(index);
}

static bool grep_option_seen(p8 letter, string_address value)
{
        if (letter == 'E')
                grep_extended = true;
        else if (letter == 'G')
                grep_extended = false;
        else if (letter == 'F')
                grep_fixed = true;
        else if (letter == 'i' || letter == 'y')
                grep_icase = true;
        else if (letter == 'M')
                grep_icase = false;
        else if (letter == 'e')
        {
                grep_pattern_add(value, string_length(value), grep_fixed,
                                 grep_extended);
                grep_said_pattern = true;
        }
        else if (letter == 'Q')
        {
                if (!grep_glob_add(address_of grep_include, value,
                                   string_length(value), false))
                        return false;
        }
        else if (letter == 'S')
        {
                if (!grep_glob_add(address_of grep_exclude, value,
                                   string_length(value), false))
                        return false;
        }
        else if (letter == 'V')
        {
                if (!grep_glob_add(address_of grep_exclude_dir, value,
                                   string_length(value), true))
                        return false;
        }
        else if (letter == 'X')
        {
                if (!text_open(value))
                        return false;

                while (text_line_next())
                        if (!grep_glob_add(address_of grep_exclude,
                                           (string_address)text_line,
                                           text_line_length, false))
                        {
                                text_close();
                                return false;
                        }

                text_close();

                if (text_status)
                        return false;
        }
        else if (letter == 'f')
        {
                if (!text_open(value))
                        return false;

                // An empty pattern file matches nothing at all, which is not
                // the same as an empty pattern.
                while (text_line_next())
                        grep_pattern_add(text_line, text_line_length, grep_fixed,
                                         grep_extended);

                text_close();

                if (!grep_pattern_any)
                        grep_never = true;

                grep_said_pattern = true;
        }

        return true;
}

// A word one of the three options takes and only one of the words will do.
static bool grep_word_is(string_address value, string_address first,
                         string_address second, string_address third)
{
        return string_equals(value, first) || string_equals(value, second) ||
               (third && string_equals(value, third));
}

static b32 text_grep()
{
        file_taking taking = {
            .program = (string_address) "grep",
            // -y is -i said the old way. Everything here is bytes already:
            // -a says read a binary file as text, -I and -U say what to do
            // about the ones that are not, and neither describes anything
            // this does.
            .allowed = (string_address) "ABCDEFGHILRTUZabcdefhilmnoqrsvwxyz",
            .valued = (string_address) "ABCDJNOQSVXdefm",
            .optional = (string_address) "W",
            .longs = grep_longs,
            .operand = grep_operand,
            .seen = grep_option_seen,
        };

        text_begin("grep");

        grep_fixed = false;
        grep_extended = false;
        grep_icase = false;
        grep_never = false;
        grep_said_pattern = false;
        grep_pattern_from = -1;
        grep_pattern_broken = false;
        grep_pattern_length = 0;
        grep_pattern_any = false;
        grep_pattern_groups = 0;
        grep_include = null;
        grep_exclude = null;
        grep_exclude_dir = null;
        grep_path_count = 0;
        grep_expanded = false;
        grep_skip_directories = false;
        grep_coloring = false;
        grep_color_ne = false;
        grep_color_reverse = false;
        grep_colors = null;
        grep_match_slot = 0;
        grep_hold_color = null;
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(2);

        if (!text_files_ready())
                return text_done(2);

        positive flags = taking.flags;
        bool extended = grep_extended;
        bool fixed = grep_fixed;
        bool never = grep_never;
        bool have_pattern = grep_said_pattern;
        bool icase = grep_icase;
        bool invert = (flags & FILE_FLAG('v')) != 0;
        bool counting = (flags & FILE_FLAG('c')) != 0;
        bool listing = (flags & FILE_FLAG('l')) != 0;
        bool listing_without = (flags & FILE_FLAG('L')) != 0;
        bool quiet = (flags & FILE_FLAG('q')) != 0;
        bool no_names = (flags & FILE_FLAG('h')) != 0;
        bool with_names = (flags & FILE_FLAG('H')) != 0;
        bool quietly = (flags & FILE_FLAG('s')) != 0;
        bool whole_line = (flags & FILE_FLAG('x')) != 0;
        bool whole_word = (flags & FILE_FLAG('w')) != 0;
        bool only = (flags & FILE_FLAG('o')) != 0;
        bool null_data = (flags & FILE_FLAG('z')) != 0;
        positive limit = TEXT_UNSET;
        positive before = 0;
        positive after = 0;
        string_address label = file_option_value(address_of taking, 'J');
        string_address separator = file_option_value(address_of taking, 'O');
        b32 pattern_from = grep_pattern_from;
        string_address said;

        if (!separator)
                separator = (flags & FILE_FLAG('P')) ? null : (string_address) "--";

        grep_numbered = (flags & FILE_FLAG('n')) != 0;
        grep_offsets = (flags & FILE_FLAG('b')) != 0;
        grep_tabbed = (flags & FILE_FLAG('T')) != 0;
        grep_null_name = (flags & FILE_FLAG('Z')) != 0;
        grep_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R'))) != 0;
        grep_dereference = (flags & FILE_FLAG('R')) != 0;

        /*
                -d, -D and --binary-files all name a kind of file to do
                something other than read. Nothing here is ever handed a
                directory or a device by the shell that is not, and every file
                is bytes, so the answer is the same whichever word came -- but
                the word still has to be one of the words, because a script
                that misspells it is told so by GNU.

                The three exit statuses below are not a pattern. They are what
                grep 3.11 did.
        */
        said = file_option_value(address_of taking, 'd');

        if (said)
        {
                if (string_equals(said, "recurse"))
                        grep_recursive = true;
                else if (string_equals(said, "skip"))
                        grep_skip_directories = true;
                else if (!string_equals(said, "read"))
                        return text_refuse(
                            said, "invalid argument for --directories", 1);
        }

        said = file_option_value(address_of taking, 'D');

        if (said && !grep_word_is(said, "read", "skip", null))
                return text_refuse(null, "unknown devices method", 2);

        said = file_option_value(address_of taking, 'N');

        if (said && !grep_word_is(said, "binary", "text", "without-match"))
                return text_refuse(null, "unknown binary-files type", 2);

        said = file_option_value(address_of taking, 'W');

        if (flags & FILE_FLAG('W'))
        {
                b32 when = file_color_when(said, FILE_COLOR_AUTO);

                if (when < 0)
                        return text_refuse(said,
                                           "invalid argument for --color", 2);

                grep_coloring = file_color_active(when);
                grep_colors = file_environment((string_address) "GREP_COLORS");
                grep_color_ne = file_color_has(grep_colors, (string_address) "ne");
                grep_color_reverse = invert &&
                                     file_color_has(grep_colors,
                                                    (string_address) "rv");
        }

        for (positive k = 0; k < 4; k++)
        {
                // -C is both sides at once, and an -A or a -B beside it is
                // the side that was named twice.
                p8 letter = k == 0 ? 'm' : k == 1 ? 'C' : k == 2 ? 'A' : 'B';
                positive number = 0;

                said = file_option_value(address_of taking, letter);

                if (!said)
                        continue;

                if (!string_digits_exact(said, address_of number))
                        return text_refuse(
                            null, "invalid context length argument", 2);

                if (letter == 'm')
                        limit = number;
                else if (letter == 'A')
                        after = number;
                else if (letter == 'B')
                        before = number;
                else
                        after = before = number;
        }

        if (pattern_from >= 0 && !grep_pattern_any && !never)
        {
                string_address value = program_argument(pattern_from);

                grep_pattern_add(value, string_length(value), fixed, extended);
        }

        if (grep_pattern_broken)
                return text_refuse(null, "pattern too long", 2);

        if (text_status)
                return text_done(2);

        if (!have_pattern)
                return text_refuse(null, "no pattern given", 2);

        // -x and -w are the pattern with something wrapped around it, which
        // is cheaper than a second answer from the machine.
        if ((whole_line || whole_word) && !never)
        {
                // Taken before the anchors go on: a line without the fixed
                // string cannot match with them either, and the wrapped
                // pattern is no longer a fixed string to look at.
                if (regex_compile(grep_pattern, extended, icase, false,
                                  REGEX_POLICY_DEFAULT))
                        grep_literal_keep();

                p8 around[GREP_PATTERN_MAX];
                string_address head = whole_line ? (extended ? "^(" : "^\\(")
                                                 : (extended ? "(^|\\W)(" : "\\(^\\|\\W\\)\\(");
                string_address tail = whole_line ? (extended ? ")$" : "\\)$")
                                                 : (extended ? ")(\\W|$)" : "\\)\\(\\W\\|$\\)");
                positive head_length = string_length(head);
                positive tail_length = string_length(tail);
                positive have = head_length + grep_pattern_length + tail_length;

                if (have >= GREP_PATTERN_MAX)
                        return text_refuse(null, "pattern too long", 2);

                memory_copy_apart(around, head, head_length);
                memory_copy_apart(around + head_length, grep_pattern,
                                 grep_pattern_length);
                // The wrapper opens one group for -x and two for -w before
                // the pattern's own, so its references move along by that.
                grep_shift_references(around + head_length, grep_pattern_length,
                                      whole_line ? 1 : 2, extended);
                memory_copy_apart(around + head_length + grep_pattern_length,
                                 tail, tail_length);

                around[have] = '\0';
                memory_copy(grep_pattern, around, have + 1);
                grep_pattern_length = have;

                if (whole_word)
                        grep_match_slot = 4;
        }

        if (!never && !regex_compile(grep_pattern, extended, icase, false,
                                     REGEX_POLICY_DEFAULT))
                return text_refuse(null, "invalid regular expression", 2);

        if (!whole_line && !whole_word)
                grep_literal_keep();

        grep_literal_icase = icase;

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

        // Room for what was named, and for a walk's worth of names when -r
        // asked for one: nothing at all when the input is standard input,
        // which is the arena never being touched by the usual grep.
        grep_paths_room = grep_recursive ? GREP_PATHS_MAX
                                         : (positive)text_files_count;

        if (grep_paths_room)
        {
                grep_paths = (string_address address_to)text_arena_take(
                    grep_paths_room * sizeof(string_address));

                if (!grep_paths)
                        return text_done(2);
        }

        if (grep_recursive && !text_files_count)
        {
                grep_expanded = true;
                grep_walk((string_address) "", 0);
        }

        for (b32 i = 0; i < text_files_count; i++)
        {
                string_address name = program_argument(text_files[i]);
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
                        grep_walk(name, 0);
                        continue;
                }

                if (grep_wanted_file(name))
                        grep_path_add(name);
        }

        b32 inputs = from_stdin ? 1 : (b32)grep_path_count;

        // How many files were named, not how many are left after --include
        // took some away: GNU decides whether to print a name before it has
        // decided which files it is going to read.
        grep_names = (text_files_count > 1 || with_names || grep_expanded) && !no_names;

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
                if (name && text_directory(text_input.handle))
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
                                grep_column = positive_digits(size);
                }

                grep_hold_clear();

                // -v wants the lines that do not match and the context flags
                // want the ones around them, so neither can have any line go
                // by unread.
                bool skipping = grep_literal_length && !invert && !before && !after;
                bool direct_counting = counting && skipping && !whole_line &&
                                       !whole_word && !listing &&
                                       !listing_without && !quiet;
                bool fused_counting = direct_counting && !icase &&
                                      limit == TEXT_UNSET;

                for (;;)
                {
                        // The line skipping stopped on holds the fixed string
                        // already, and asking the machine again would be the
                        // same search a second time -- unless -x or -w put
                        // something around it, or the line was too long to
                        // arrive whole.
                        bool sure = false;

                        /*
                                The fixed, case-sensitive count is one scan,
                                not one prepared search plus one newline hunt
                                per matching record. Only whole records are
                                handed to the library. The trailing partial
                                one stays for the line reader, which carries
                                it across a refill.
                        */
                        if (fused_counting && text_fill())
                        {
                                p8 address_to at = text_input.buffer +
                                                   text_input.position;
                                positive left = text_input.filled -
                                                text_input.position;
                                p8 address_to last = memory_last_of(
                                    at, text_delimiter, left);

                                if (last)
                                {
                                        positive take = (positive)(last - at) + 1;
                                        positive got =
                                            memory_count_records_with_prepared(
                                                at, take, grep_literal,
                                                grep_literal_length,
                                                grep_literal_anchors.x,
                                                grep_literal_anchors.y,
                                                text_delimiter);

                                        matches += got;
                                        found_any = found_any || got;
                                        text_input.position += take;
                                        continue;
                                }
                        }

                        if (direct_counting && grep_skip(null, null))
                        {
                                if (!grep_discard_line())
                                        break;

                                matches++;
                                found_any = true;

                                if (limit != TEXT_UNSET && matches >= limit)
                                        break;

                                continue;
                        }

                        if (skipping && !direct_counting)
                        {
                                positive jumped = 0;

                                sure = grep_skip(address_of number, address_of jumped) &&
                                       !whole_line && !whole_word;
                                offset += jumped;
                        }

                        if (!text_line_next())
                                break;

                        number++;

                        positive at = offset;

                        offset += text_line_length + (text_line_ended ? 1 : 0);

                        bool hit = !never &&
                                   ((sure && text_line_length < TEXT_LINE_MAX) ||
                                    regex_search(text_line, text_line_length, 0));

                        if (hit == invert)
                        {
                                if (pending)
                                {
                                        grep_group_gap(grouped, separator,
                                                       address_of split, shown,
                                                       number);
                                        grep_head(shown_name, '-', number, at);
                                        grep_color_line(text_line, text_line_length,
                                                        true, invert);
                                        text_put_character(text_delimiter);

                                        shown = number;
                                        shown_any = true;
                                        pending--;
                                }
                                else if (before)
                                {
                                        if (!grep_hold_put(text_line, text_line_length,
                                                           number))
                                        {
                                                trouble = 2;
                                                break;
                                        }
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

                                        grep_group_gap(grouped, separator,
                                                       address_of split, shown,
                                                       n);
                                        grep_head(shown_name, '-', n, 0);
                                        grep_hold_say(slot, invert);
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
                                        positive whole_stop = regex_slots[1];
                                        positive begin = regex_slots[grep_match_slot];
                                        positive stop = regex_slots[grep_match_slot + 1];

                                        if (stop == begin)
                                        {
                                                from = begin + 1;
                                                continue;
                                        }

                                        if (!invert)
                                        {
                                                grep_head(shown_name, ':', number, at + begin);
                                                grep_color_field(
                                                    text_line + begin, stop - begin,
                                                    (string_address) "ms",
                                                    (string_address) "01;31");
                                                text_put_character(text_delimiter);
                                        }

                                        from = grep_match_slot ? stop : whole_stop;
                                }

                                shown = number;
                                shown_any = true;
                                pending = after;

                                if (limit != TEXT_UNSET && matches >= limit && !pending)
                                        break;

                                continue;
                        }

                        grep_group_gap(grouped, separator, address_of split,
                                       shown, number);
                        grep_head(shown_name, ':', number, at);
                        grep_color_line(text_line, text_line_length, false,
                                        !invert);
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
                                grep_color_field(shown_name,
                                                 string_length(shown_name),
                                                 (string_address) "fn",
                                                 (string_address) "35");
                                text_put_character(grep_null_name ? '\0' : '\n');
                        }

                        continue;
                }

                if (counting && !quiet)
                {
                        if (grep_names)
                        {
                                grep_color_field(shown_name,
                                                 string_length(shown_name),
                                                 (string_address) "fn",
                                                 (string_address) "35");

                                if (grep_null_name)
                                        text_put_character('\0');
                                else
                                        grep_color_separator(':');
                        }

                        positive_to_string(text_put, matches);
                        text_put_character('\n');
                }
        }

        // Two for any trouble, including a read that failed or an expression
        // the matcher gave up on, as the reference grep answers.
        if (trouble || text_status)
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
#define SED_APPENDS_MAX SED_COMMANDS_MAX

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

#define sed_script_put(character)                                            \
        fixed_store_byte(sed_script, sed_script_length, sed_broken, character)

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

        sed_text_used = (positive)(memory_copy_apart_end(
            sed_text + sed_text_used, from, length) - sed_text) + 1;
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
static bool sed_io_failed;
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

                if (!string_compare_max(had, name, length) && !had[length])
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

/*
        Whether a space has room for more bytes. The pattern and hold spaces
        are one line's worth each, which is what every command but three
        needs; N, H and G grow them, and s can grow the pattern space by the
        length of its replacement at every match. A script that slurps a
        whole file with N used to write past the end of the array and on
        into whatever followed it. What does not fit is refused, with the
        status sed keeps for its own failures.
*/
static bool sed_space_full;

static bool sed_work_byte(positive address_to have, p8 value);

static bool sed_space_fits(positive have, positive more)
{
        if (more <= TEXT_LINE_MAX - have)
                return true;

        if (!sed_space_full)
                text_error(null, "pattern space too large");

        sed_space_full = true;
        return false;
}

static bool sed_work_byte(positive address_to have, p8 value)
{
        if (!sed_space_fits(address_to have, 1))
                return false;

        sed_work[(address_to have)++] = value;
        return true;
}

// Whether the line just read was the last: of everything, or of this file
// when -s makes each file its own stream. n and N used to ask without the
// -s half and never saw the last line of any file but the last.
static bool sed_input_ends(b32 i, b32 inputs)
{
        return !text_fill() && (sed_separate || i == inputs - 1);
}

/*
        The next line for N, across a file boundary when the input is one
        stream. Without -s the reference sed joins the last line of one file
        to the first of the next, and N used to fail at every file's end,
        pairing the lines of each file from its own first line. Under -s and
        -i each file is its own stream, and the boundary is an end.
*/
static string_address sed_in_place;

static bool sed_line_across(b32 address_to i, b32 inputs)
{
        while (!text_line_next())
        {
                if (sed_separate || sed_in_place || address_to i + 1 >= inputs)
                        return false;

                text_close();
                (address_to i)++;

                if (!text_open(text_file_name(address_to i)))
                        text_status = 2;
        }

        return true;
}
static bool sed_null_data;
static string_address sed_in_place;
static bool sed_follow_symlinks;

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

        if (!regex_compile(pattern, sed_extended, icase, true,
                           REGEX_POLICY_DEFAULT))
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
        sed_at += string_span_max(sed_script + sed_at,
                                  sed_script_length - sed_at, string_set_blanks);
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
                                else
                                        sed_broken = true;

                                sed_at += 2;
                                continue;
                        }

                        if (have < room - 2)
                        {
                                into[have++] = '\\';
                                into[have++] = sed_script[sed_at + 1];
                        }
                        else
                                sed_broken = true;

                        sed_at += 2;
                        continue;
                }

                // A piece longer than its room breaks the script rather than
                // fitting: cut, a pattern compiles to some other pattern and
                // matches the wrong lines without a word.
                if (have < room - 1)
                        into[have++] = sed_script[sed_at];
                else
                        sed_broken = true;

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
        if (sed_at >= sed_script_length)
                return 0;

        positive taken;
        positive value = string_digits_max(sed_script + sed_at,
                                            sed_script_length - sed_at,
                                            address_of taken);

        sed_at += taken;
        return value;
}

// Everything left on the line, which is what a file name is to r and w: a
// semicolon in one is part of the name and not the end of the command.
static positive sed_rest_of_line(p8 address_to into, positive room)
{
        sed_skip_blanks();

        positive left = sed_script_length - sed_at;
        p8 address_to from = sed_script + sed_at;
        p8 address_to newline = memory_first_of(from, '\n', left);
        positive length = newline ? (positive)(newline - from) : left;
        positive have = min(length, room - 1);

        memory_copy_apart_end(into, from, have);
        sed_at += length;
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

        while (have && byte_is_blank(into[have - 1]))
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

        if (byte_is_digit(character))
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
                                else if (byte_is_digit(flag))
                                {
                                        command->which = sed_number_at();
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
                                                if (have < sizeof(body) - 1)
                                                        body[have++] = '\n';
                                                else
                                                        sed_broken = true;

                                                sed_at++;
                                                continue;
                                        }
                                }

                                // A body longer than its room is not cut
                                // to fit: a script that lost its tail would
                                // write the wrong text and say nothing.
                                if (have < sizeof(body) - 1)
                                        body[have++] = sed_script[sed_at];
                                else
                                        sed_broken = true;

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

        if (system_write_all((positive)sed_files[which].handle, sed_space,
                             sed_space_length) != sed_space_length)
        {
                sed_io_failed = true;
                return;
        }

        // A last line that came without one does not leave with one.
        if (sed_space_ended)
                if (system_write_all((positive)sed_files[which].handle,
                                     address_of mark, 1) != 1)
                        sed_io_failed = true;
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
                bipolar got = system_read_retry((positive)handle, window,
                                                sizeof(window));

                if (got <= 0)
                        break;

                text_put(window, (positive)got);
        }

        system_close(handle);
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

                if (!sed_space_fits(have, from - at))
                        return false;

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

                        if (!sed_work_byte(address_of have, sed_space[from]))
                                        return false;
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

                                        if (byte_is_digit(next))
                                        {
                                                copy_from = regex_slots[(next - '0') * 2];
                                                copy_to = regex_slots[(next - '0') * 2 + 1];

                                                if (copy_from == TEXT_UNSET ||
                                                    copy_to == TEXT_UNSET)
                                                        continue;
                                        }
                                        else
                                        {
                                                p8 escaped = next == 'n'   ? '\n'
                                                             : next == 't' ? '\t'
                                                             : next == 'r' ? '\r'
                                                                           : next;

                                                if (!sed_work_byte(address_of have,
                                                                   escaped))
                                                        return false;

                                                continue;
                                        }
                                }
                                else
                                {
                                        if (!sed_work_byte(address_of have, character))
                                        return false;
                                        continue;
                                }

                                if (!sed_space_fits(have, copy_to - copy_from))
                                        return false;

                                memory_copy(sed_work + have, sed_space + copy_from,
                                            copy_to - copy_from);
                                have += copy_to - copy_from;
                        }

                        changed = true;
                }
                else
                {
                        if (!sed_space_fits(have, to - from))
                                return false;

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
                                if (!sed_work_byte(address_of have, sed_space[from]))
                                        return false;

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
                if (!sed_space_fits(have, sed_space_length - at))
                        return false;

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
        and then a trace of every line through it. --follow-symlinks resolves
        an in-place input before the temporary is named, so the final rename
        replaces the target and leaves the link itself alone.
*/
// P is a letter sed has not got, and is where the two words that take away
// what is not here to take go.
static const file_long sed_longs[] = {
    {(string_address) "quiet", 'n'},
    {(string_address) "silent", 'n'},
    {(string_address) "expression", 'e'},
    {(string_address) "file", 'f'},
    {(string_address) "in-place", 'i'},
    {(string_address) "line-length", 'l'},
    {(string_address) "regexp-extended", 'E'},
    {(string_address) "separate", 's'},
    {(string_address) "unbuffered", 'u'},
    {(string_address) "null-data", 'z'},
    {(string_address) "posix", 'P'},
    {(string_address) "sandbox", 'P'},
    {(string_address) "follow-symlinks", 'F'},
    {null, 0},
};

// The first word that is not an option is the script, and every word after it
// is a file. -e and -f come as often as there are pieces of script, which is
// what the callback below is for.
static bool sed_have_script;
static b32 sed_option_status;

static fn sed_operand(b32 index)
{
        if (sed_have_script)
        {
                text_file_add(index);
                return;
        }

        sed_script_add(program_argument(index));
        sed_have_script = true;
}

static bool sed_option_seen(p8 letter, string_address value)
{
        if (letter == 'e')
        {
                sed_script_add(value);
                sed_have_script = true;
                return true;
        }

        if (letter != 'f')
                return true;

        // Four, not one: sed keeps its usage errors and its I/O failures
        // apart, and a script file that is not there is the second kind.
        if (!text_open(value))
        {
                sed_option_status = 4;
                return false;
        }

        while (text_line_next())
        {
                text_line[text_line_length] = '\0';
                sed_script_add(text_line);
        }

        text_close();
        sed_have_script = true;

        return true;
}

static b32 text_sed()
{
        b32 leaving = -1;
        file_taking taking = {
            .program = (string_address) "sed",
            // -u asks for output a line at a time, which costs something only
            // when somebody is reading it live. -l is how wide the l command
            // wraps, and there is no l command here to wrap.
            .allowed = (string_address) "Eefilnrsuz",
            .valued = (string_address) "efl",
            // -i takes its suffix joined on -- sed -in is in place with a
            // backup called n, not -i -n.
            .optional = (string_address) "i",
            .longs = sed_longs,
            .operand = sed_operand,
            .seen = sed_option_seen,
        };

        text_begin("sed");

        sed_have_script = false;
        sed_option_status = 1;
        sed_broken = false;
        sed_io_failed = false;

        if (!file_take(address_of taking))
                return text_done(sed_option_status);

        if (!text_files_ready())
                return text_done(1);

        positive flags = taking.flags;
        bool have_script = sed_have_script;

        sed_quiet = (flags & FILE_FLAG('n')) != 0;
        sed_extended = (flags & (FILE_FLAG('r') | FILE_FLAG('E'))) != 0;
        sed_separate = (flags & FILE_FLAG('s')) != 0;
        sed_null_data = (flags & FILE_FLAG('z')) != 0;
        sed_follow_symlinks = (flags & FILE_FLAG('F')) != 0;

        if (flags & FILE_FLAG('i'))
        {
                string_address suffix = file_option_value(address_of taking, 'i');

                sed_in_place = suffix ? suffix : (string_address) "";
                sed_separate = true;
        }

        if (!have_script)
                return text_refuse(null, "no script", 1);

        // After the script has been read, not while: -f reads its file with
        // the same reader and a script is lines however the input is split.
        if (sed_null_data)
                text_delimiter = '\0';

        sed_parse();

        if (sed_broken)
                return text_refuse(null, "unsupported or invalid script",
                                   sed_broken_status);

        // -i edits files, and there is nothing to edit when the input is a
        // pipe. GNU says so and stops with four.
        if (sed_in_place && !text_files_count)
                return text_refuse(null, "no input files", 4);

        b32 inputs = text_input_count();
        positive temporary_nonce = sed_in_place
            ? (positive)system_call_1(syscall(getpid), 0) * 31
            : 0;

        for (b32 i = 0; i < inputs && leaving < 0; i++)
        {
                string_address name = text_file_name(i);
                p8 resolved[TEXT_PATH_MAX];
                p8 temporary[TEXT_PATH_MAX];
                bipolar written = -1;

                if (sed_in_place && sed_follow_symlinks)
                {
                        if (!file_resolve(name, resolved, true))
                        {
                                text_error(name, "cannot follow symbolic link");
                                text_status = 4;
                                continue;
                        }

                        name = (string_address)resolved;
                }

                if (!text_open(name))
                {
                        text_status = 2;
                        continue;
                }

                if (sed_in_place)
                {
                        written = file_temporary_open(
                            name, temporary, TEXT_PATH_MAX,
                            (string_address)"sed", 3,
                            temporary_nonce + (positive)i * 64, 64, 0600);

                        if (written < 0)
                        {
                                text_close();
                                if (!temporary[0])
                                        return text_refuse(
                                            name,
                                            "cannot make a temporary file beside",
                                            4);
                                return text_refuse(temporary, "cannot create", 4);
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
                        sed_last = sed_input_ends(i, inputs);

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
                                        bool did = sed_substitute(command);

                                        if (sed_space_full)
                                        {
                                                leaving = 4;
                                                dropped = true;
                                                break;
                                        }

                                        if (!did)
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
                                        memory_translate(sed_space, sed_space_length,
                                                         sed_maps[command->map]);

                                        continue;
                                }

                                if (kind == 'p')
                                {
                                        sed_put_space();
                                        continue;
                                }

                                if (kind == 'P')
                                {
                                        p8 address_to newline =
                                            memory_first_of(sed_space, '\n',
                                                            sed_space_length);
                                        positive stop = newline
                                                              ? (positive)(newline - sed_space)
                                                              : sed_space_length;

                                        text_put(sed_space, stop);

                                        if (newline)
                                                text_put_character('\n');
                                        else if (sed_space_ended)
                                                text_put_character(text_delimiter);
                                        continue;
                                }

                                if (kind == '=')
                                {
                                        positive_to_string(text_put, sed_number);
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
                                        p8 address_to newline =
                                            memory_first_of(sed_space, '\n',
                                                            sed_space_length);
                                        positive stop = newline
                                                              ? (positive)(newline - sed_space)
                                                              : sed_space_length;

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
                                        sed_last = sed_input_ends(i, inputs);
                                        continue;
                                }

                                if (kind == 'N')
                                {
                                        if (!sed_line_across(address_of i, inputs))
                                        {
                                                sed_last = true;
                                                break;
                                        }

                                        if (!sed_space_fits(sed_space_length,
                                                            text_line_length + 1))
                                        {
                                                leaving = 4;
                                                dropped = true;
                                                break;
                                        }

                                        sed_space[sed_space_length++] = '\n';
                                        memory_copy(sed_space + sed_space_length, text_line,
                                                    text_line_length);
                                        sed_space_length += text_line_length;
                                        sed_space_ended = text_line_ended;
                                        sed_number++;
                                        sed_last = sed_input_ends(i, inputs);
                                        continue;
                                }

                                if (kind == 'h' || kind == 'H')
                                {
                                        if (kind == 'H' &&
                                            !sed_space_fits(sed_hold_length,
                                                            sed_space_length + 1))
                                        {
                                                leaving = 4;
                                                dropped = true;
                                                break;
                                        }

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
                                        if (kind == 'G' &&
                                            !sed_space_fits(sed_space_length,
                                                            sed_hold_length + 1))
                                        {
                                                leaving = 4;
                                                dropped = true;
                                                break;
                                        }

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

                        if (sed_failed || sed_io_failed)
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

                        if (leaving >= 0 || sed_failed || sed_io_failed)
                                break;
                }

                text_close();

                if (written >= 0)
                {
                        bipolar closed;

                        text_flush();
                        text_out_to(1);
                        closed = system_close(written);

                        /* Never replace the input with a partial temporary. */
                        if (text_out_failed || closed < 0)
                        {
                                system_remove_at(AT_FDCWD,
                                              temporary, 0);
                                text_error(name, "write error");
                                text_status = 4;
                                break;
                        }

                        if (sed_in_place[0])
                        {
                                p8 kept[TEXT_PATH_MAX];
                                positive length = string_length(name);
                                positive extra = string_length(sed_in_place);

                                if (length + extra + 1 < TEXT_PATH_MAX)
                                {
                                        memory_copy(kept, name, length);
                                        memory_copy_apart_end(
                                            kept + length, sed_in_place, extra);
                                        system_rename_at(AT_FDCWD, name,
                                                         AT_FDCWD, kept, 0);
                                }
                        }

                        system_rename_at(AT_FDCWD, temporary, AT_FDCWD, name, 0);
                }

                if (sed_failed || sed_io_failed)
                        break;
        }

        for (b32 c = 0; c < sed_file_count; c++)
                if (sed_files[c].handle >= 0)
                        if (system_close(sed_files[c].handle) < 0)
                                sed_io_failed = true;

        if (sed_io_failed)
                return text_refuse(null, "write error", 4);

        if (sed_failed)
                return text_refuse(null, "no previous regular expression", 1);

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
        // Whether the key spelled any ordering option of its own. One that
        // did takes nothing from the command line: -r -k1n sorts the key
        // numerically and forwards, as the reference sort does.
        bool ordered;
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

static positive sort_separator_from(p8 address_to at, positive length, positive from)
{
        p8 address_to found = memory_first_of(at + from, sort_separator,
                                              length - from);

        return found ? (positive)(found - at) : length;
}

static inline INLINE positive sort_field_edge(p8 address_to at,
                                              positive length,
                                              positive field, bool stop)
{
        positive scan = 0;
        positive first = stop ? 0 : 1;

        if (sort_have_separator)
        {
                for (positive i = first; i < field && scan < length; i++)
                {
                        scan = sort_separator_from(at, length, scan);

                        if (scan < length && (!stop || i + 1 < field))
                                scan++;
                }

                return scan;
        }

        for (positive i = first; i < field && scan < length; i++)
        {
                scan += string_span_max(at + scan, length - scan, string_set_blanks);
                scan += string_span_max(at + scan, length - scan, text_inside());
        }

        return scan;
}

#define sort_field_start(at, length, field)                                 \
        sort_field_edge((at), (length), (field), false)
#define sort_field_stop(at, length, field)                                  \
        sort_field_edge((at), (length), (field), true)

static fn sort_key_span(sort_key address_to key, p8 address_to at, positive length,
                        positive address_to from, positive address_to to)
{
        positive begin = 0;
        positive finish = length;

        if (key->first_field)
        {
                begin = sort_field_start(at, length, key->first_field);

                if (key->skip_blanks_first)
                        begin += string_span_max(at + begin, length - begin, string_set_blanks);

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
                                finish += string_span_max(at + finish, length - finish,
                                                          string_set_blanks);

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
static positive sort_zero_prefix(p8 address_to text, positive from, positive stop)
{
        positive length = stop - from;

        if (length >= 64)
                return from + memory_span_byte(text + from, '0', length);

        while (from < stop && text[from] == '0')
                from++;

        return from;
}

static PURE bipolar sort_compare_number(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        positive at_a = 0, at_b = 0;
        bool minus_a = false, minus_b = false;

        at_a = string_span_max(a, la, string_set_blanks);
        at_b = string_span_max(b, lb, string_set_blanks);

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

        while (at_a < la && byte_is_digit(a[at_a]))
                at_a++;

        while (at_b < lb && byte_is_digit(b[at_b]))
                at_b++;

        positive int_a_stop = at_a, int_b_stop = at_b;
        positive frac_a = at_a, frac_a_stop = at_a;
        positive frac_b = at_b, frac_b_stop = at_b;

        if (at_a < la && a[at_a] == '.')
        {
                frac_a = ++at_a;

                while (at_a < la && byte_is_digit(a[at_a]))
                        at_a++;

                frac_a_stop = at_a;
        }

        if (at_b < lb && b[at_b] == '.')
        {
                frac_b = ++at_b;

                while (at_b < lb && byte_is_digit(b[at_b]))
                        at_b++;

                frac_b_stop = at_b;
        }

        int_a = sort_zero_prefix(a, int_a, int_a_stop);
        int_b = sort_zero_prefix(b, int_b, int_b_stop);

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

static bool sort_looked_at(p8 character, positive how)
{
        // -d keeps blanks and alphanumerics; -i keeps what a terminal would
        // show. A byte neither keeps is not there at all, so the two sides
        // walk at their own pace rather than in step.
        // Alphanumeric, not a word character: sort -d keeps no underscore,
        // which is what separates it from every other definition here.
        if ((how & SORT_DICTIONARY) &&
            !(byte_is_blank(character) || byte_is_alnum(character)))
                return false;

        if ((how & SORT_PRINTABLE) && (character < 0x20 || character >= 0x7f))
                return false;

        return true;
}

static PURE bipolar sort_compare_bytes(p8 address_to a, positive la, p8 address_to b, positive lb,
                                  positive how)
{
        if (!how)
        {
                positive length = min(la, lb);
                bipolar order = memory_compare(a, b, length);

                if (order)
                        return order < 0 ? -1 : 1;

                return la == lb ? 0 : la < lb ? -1 : 1;
        }

        if (how == SORT_FOLD)
        {
                positive length = min(la, lb);
                b32 order = memory_compare_ascii_case(a, b, length);

                if (order)
                        return order < 0 ? -1 : 1;

                return la == lb ? 0 : la < lb ? -1 : 1;
        }

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

        scan = string_span_max(at, length, string_set_blanks);

        // Only a minus: GNU reads +7 as no number at all, here as in every
        // other number this file reads.
        if (scan < length && at[scan] == '-')
        {
                sign = -1;
                scan++;
        }

        // The increment is its own statement. Written into the test it stops
        // happening the moment nonzero is true, and the loop never ends.
        while (scan < length && byte_is_digit(at[scan]))
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

                while (scan < length && byte_is_digit(at[scan]))
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

static PURE bipolar sort_compare_human(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        bipolar one = sort_human_order(a, la);
        bipolar two = sort_human_order(b, lb);

        if (one != two)
                return one < two ? -1 : 1;

        return sort_compare_number(a, la, b, lb);
}

// -M, in the C locale, which is the only one this has. Anything that is not
// one of the twelve is month zero and ties with every other such line, so the
// last resort is what actually orders the text. The names are date's, read
// three letters deep without regard to case.

static b32 sort_month_of(p8 address_to at, positive length)
{
        positive scan = 0;

        scan = string_span_max(at, length, string_set_blanks);

        if (length - scan < 3)
                return 0;

        for (b32 m = 0; m < 12; m++)
        {
                if (!memory_compare_ascii_case(at + scan, file_month_names[m], 3))
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
        if (byte_is_digit(character))
                return 0;

        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z'))
                return (b32)character;

        if (character == '~')
                return -1;

        return (b32)character + 256;
}

static PURE bipolar sort_version_walk(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        positive i = 0;
        positive j = 0;

        while (i < la || j < lb)
        {
                bipolar first = 0;

                while ((i < la && !byte_is_digit(a[i])) ||
                       (j < lb && !byte_is_digit(b[j])))
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

                while (i < la && j < lb && byte_is_digit(a[i]) &&
                       byte_is_digit(b[j]))
                {
                        if (!first)
                                first = (bipolar)a[i] - (bipolar)b[j];

                        i++;
                        j++;
                }

                if (i < la && byte_is_digit(a[i]))
                        return 1;

                if (j < lb && byte_is_digit(b[j]))
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

static PURE positive sort_version_stem(p8 address_to at, positive length)
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

                        if (c >= length || byte_is_digit(at[c]) ||
                            !sort_version_tail(at[c]))
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

static PURE bipolar sort_compare_version(p8 address_to a, positive la, p8 address_to b, positive lb)
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

static PURE bipolar sort_compare_kind(p8 kind, positive how, p8 address_to a, positive la,
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

static PURE HOT bipolar sort_compare_keys(positive left, positive right)
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
                        from_a = string_span_max(a->at, a->length,
                                                 string_set_blanks);
                        from_b = string_span_max(b->at, b->length,
                                                 string_set_blanks);
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
static PURE HOT bipolar sort_compare(positive left, positive right)
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

static positive sort_radix_key(positive line, positive depth)
{
        text_slice address_to item = text_lines + line;

        return depth < item->length ? (positive)item->at[depth] + 1 : 0;
}

static fn sort_insertion(positive from, positive to)
{
        for (positive at = from + 1; at < to; at++)
        {
                positive value = sort_order[at];
                positive into = at;

                while (into > from &&
                       sort_compare(value, sort_order[into - 1]) < 0)
                {
                        sort_order[into] = sort_order[into - 1];
                        into--;
                }

                sort_order[into] = value;
        }
}

/*
        The ordinary C-locale sort is bytes, so comparing whole prefixes at
        every merge is avoidable. Partition on the next byte instead. Bucket
        zero is end-of-line and bytes occupy one through 256, preserving the
        exact length-aware order of memory_compare.

        The largest child is continued in this frame and only smaller children
        recurse, bounding stack depth even for adversarial tries. A single
        common-byte bucket simply advances depth in the loop, so very long
        equal prefixes do not consume stack.
*/
static fn sort_radix(positive from, positive to, positive depth)
{
        while (to - from >= 16)
        {
                positive boundary[258];
                positive next[257];
                positive largest = 0;
                positive largest_size = 0;
                positive occupied = 0;
                positive only = 0;

                memory_fill(boundary, 0, sizeof(boundary));

                for (positive at = from; at < to; at++)
                        boundary[sort_radix_key(sort_order[at], depth) + 1]++;

                boundary[0] = from;
                for (positive bucket = 0; bucket < 257; bucket++)
                {
                        if (boundary[bucket + 1])
                        {
                                occupied++;
                                only = bucket;
                        }

                        boundary[bucket + 1] += boundary[bucket];
                        next[bucket] = boundary[bucket];
                }

                /* A common byte needs no partition at all. End-of-line is
                   already fully ordered; any other byte advances the trie in
                   this frame without moving an index twice. */
                if (occupied == 1)
                {
                        if (!only)
                                return;

                        depth++;
                        continue;
                }

                /*
                        The spare index array is already paid for by every
                        sort. Distribute into it in input order, then let the
                        assembly copy return one contiguous span. The former
                        in-place cycle walk chased a different bucket on each
                        swap; on the ordinary byte sort it was the largest
                        source of cache misses and branch work.

                        This is stable within a bucket, although the default
                        sort does not require that property. More importantly,
                        both reads and writes are forward streams.
                */
                for (positive at = from; at < to; at++)
                {
                        positive line = sort_order[at];
                        positive key = sort_radix_key(line, depth);

                        sort_spare[next[key]++] = line;
                }

                memory_copy_apart(sort_order + from, sort_spare + from,
                                  (to - from) * sizeof(positive));

                for (positive bucket = 1; bucket < 257; bucket++)
                {
                        positive size = boundary[bucket + 1] -
                                        boundary[bucket];

                        if (size > largest_size)
                        {
                                largest = bucket;
                                largest_size = size;
                        }
                }

                if (largest_size < 2)
                        return;

                for (positive bucket = 1; bucket < 257; bucket++)
                        if (bucket != largest &&
                            boundary[bucket + 1] - boundary[bucket] > 1)
                                sort_radix(boundary[bucket],
                                           boundary[bucket + 1], depth + 1);

                from = boundary[largest];
                to = boundary[largest + 1];
                depth++;
        }

        sort_insertion(from, to);
}

/*
        Stable bottom-up merge sort with the two index arrays changing roles.

        The old recursive merge copied every completed run to spare and then
        copied the entire run back, doubling index traffic at every level.
        A pass already produces exactly the runs the next pass consumes, so
        it can stay where it landed. Only line indexes move; line bytes never
        do.
*/
static fn sort_run(positive count)
{
        if (count < 2)
                return;

        sort_order = array_merge_sort(sort_order, sort_spare, count,
                                      sort_compare);
}

static positive sort_key_flags(sort_key address_to key, string_address spec,
                               positive at, p8 address_to kind, bool second)
{
        while (spec[at] && (second || spec[at] != ','))
        {
                p8 option = spec[at++];

                key->ordered = true;

                if (option == 'n' || option == 'h' ||
                    option == 'M' || option == 'V')
                {
                        if (address_to kind && address_to kind != option)
                                return positive_max;

                        address_to kind = option;
                        key->kind = option;
                }
                else if (option == 'r')
                        key->reverse = true;
                else if (option == 'f')
                        key->how |= SORT_FOLD;
                else if (option == 'd')
                        key->how |= SORT_DICTIONARY;
                else if (option == 'i')
                        key->how |= SORT_PRINTABLE;
                else if (option == 'b')
                {
                        if (second)
                                key->skip_blanks_second = true;
                        else
                                key->skip_blanks_first = true;
                }
                else
                        return positive_max;
        }

        return at;
}

static bool sort_parse_key(string_address spec)
{
        if (sort_key_count >= SORT_KEYS_MAX)
                return false;

        sort_key address_to key = sort_keys + sort_key_count;
        positive at = 0;
        positive taken;
        p8 local_kind = 0;

        key->first_field = 0;
        key->first_char = 0;
        key->second_field = 0;
        key->second_char = 0;
        key->kind = 0;
        key->how = 0;
        key->reverse = false;
        key->skip_blanks_first = false;
        key->skip_blanks_second = false;
        key->ordered = false;

        key->first_field = string_digits(spec + at, address_of taken);
        at += taken;

        if (!key->first_field)
                return false;

        if (spec[at] == '.')
        {
                at++;
                key->first_char = string_digits(spec + at, address_of taken);

                if (!taken || !key->first_char)
                        return false;

                at += taken;
        }

        at = sort_key_flags(key, spec, at, address_of local_kind, false);

        if (at == positive_max)
                return false;

        if (spec[at] == ',')
        {
                at++;
                key->second_field = string_digits(spec + at, address_of taken);

                if (!taken || !key->second_field)
                        return false;

                at += taken;

                if (spec[at] == '.')
                {
                        at++;
                        key->second_char = string_digits(spec + at, address_of taken);

                        if (!taken)
                                return false;

                        at += taken;
                }

                if (sort_key_flags(key, spec, at, address_of local_kind,
                                   true) == positive_max)
                        return false;
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
// D takes a word and drops it, W is --sort, K is --check carrying one. None
// of the three is a letter sort has, so -D and -W and -K stay mistakes.
static const file_long sort_longs[] = {
    {(string_address) "ignore-leading-blanks", 'b'},
    {(string_address) "dictionary-order", 'd'},
    {(string_address) "ignore-case", 'f'},
    {(string_address) "ignore-nonprinting", 'i'},
    {(string_address) "human-numeric-sort", 'h'},
    {(string_address) "month-sort", 'M'},
    {(string_address) "numeric-sort", 'n'},
    {(string_address) "reverse", 'r'},
    {(string_address) "sort", 'W'},
    {(string_address) "check", 'K'},
    {(string_address) "version-sort", 'V'},
    {(string_address) "key", 'k'},
    {(string_address) "merge", 'm'},
    {(string_address) "output", 'o'},
    {(string_address) "stable", 's'},
    {(string_address) "buffer-size", 'D'},
    {(string_address) "field-separator", 't'},
    {(string_address) "temporary-directory", 'D'},
    {(string_address) "compress-program", 'D'},
    {(string_address) "batch-size", 'D'},
    {(string_address) "parallel", 'D'},
    {(string_address) "unique", 'u'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

// -k comes as many times as there are keys, and one value per letter cannot
// hold them: they are parsed as the options arrive.
static bool sort_key_seen(p8 letter, string_address value)
{
        if (letter != 'k')
                return true;

        if (sort_parse_key(value))
                return true;

        text_error(null, "invalid key");
        return false;
}

// "sort: -:2: disorder: apple", which is the only thing -c has to say.
static fn sort_disorder(string_address name, positive number, text_slice address_to line)
{
        text_flush();
        text_error_raw(text_name);
        text_error_raw(": ");
        text_error_raw(name);
        text_error_raw(":");

        p8 digits[24];
        positive length = positive_into(digits, number);

        system_write_all(2, digits, length);

        text_error_raw(": disorder: ");
        system_write_all(2, line->at, line->length);
        text_error_raw("\n");
}

static b32 text_sort()
{
        file_taking taking = {
            .program = (string_address) "sort",
            // -g wants a floating point number parsed, and there is no
            // floating point anywhere in this file. -S and -T tune a
            // temporary file this sort has not got.
            .allowed = (string_address) "CMSTVbcdfhikmnorstuz",
            .valued = (string_address) "DSTWkot",
            .optional = (string_address) "K",
            .longs = sort_longs,
            .operand = text_file_add,
            .seen = sort_key_seen,
        };

        text_begin("sort");

        if (!file_take(address_of taking))
                return text_done(2);

        if (!text_files_ready())
                return text_done(2);

        positive flags = taking.flags;
        bool merging = (flags & FILE_FLAG('m')) != 0;
        bool null_data = (flags & FILE_FLAG('z')) != 0;
        bool checking = (flags & (FILE_FLAG('c') | FILE_FLAG('C') |
                                  FILE_FLAG('K'))) != 0;
        bool checking_quiet = (flags & FILE_FLAG('C')) != 0;
        string_address output = file_option_value(address_of taking, 'o');
        string_address said = file_option_value(address_of taking, 'K');

        sort_reverse = (flags & FILE_FLAG('r')) != 0;
        sort_unique = (flags & FILE_FLAG('u')) != 0;
        sort_stable = (flags & FILE_FLAG('s')) != 0;
        sort_skip_blanks = (flags & FILE_FLAG('b')) != 0;

        if (flags & FILE_FLAG('f'))
                sort_how |= SORT_FOLD;

        if (flags & FILE_FLAG('d'))
                sort_how |= SORT_DICTIONARY;

        if (flags & FILE_FLAG('i'))
                sort_how |= SORT_PRINTABLE;

        /*
                Two ways of ordering the same lines is a question with no
                answer, and GNU refuses it rather than picking one. This used
                to take whichever was written last, which is a different sort
                from the one the caller asked for and no way of finding out.
        */
        for (positive k = 0; k < 4; k++)
        {
                p8 letter = k == 0 ? 'n' : k == 1 ? 'h' : k == 2 ? 'M' : 'V';

                if (!(flags & FILE_FLAG(letter)))
                        continue;

                if (sort_kind)
                        return text_refuse(null, "options are incompatible", 2);

                sort_kind = letter;
        }

        if (said)
        {
                checking_quiet = string_equals(said, "quiet") ||
                                 string_equals(said, "silent");

                if (!checking_quiet && !string_equals(said, "diagnose-first"))
                        return text_refuse(
                            said, "invalid argument for --check", 2);
        }

        said = file_option_value(address_of taking, 'W');

        if (said)
        {
                // --sort=WORD is the long options spelled a third way, and
                // the word is what the letter would have been.
                p8 kind = string_equals(said, "numeric")         ? 'n'
                          : string_equals(said, "human-numeric") ? 'h'
                          : string_equals(said, "month")         ? 'M'
                          : string_equals(said, "version")       ? 'V'
                                                                  : 0;

                /*
                        General-numeric needs floating point and random needs
                        a hash. Accepting either as ordinary byte ordering was
                        a plausible-looking answer to a different question.
                */
                if (!kind)
                        return text_refuse(said,
                                           "invalid argument for --sort", 1);

                if (sort_kind && sort_kind != kind)
                        return text_refuse(null, "options are incompatible", 2);

                sort_kind = kind;
        }

        said = file_option_value(address_of taking, 't');

        if (said)
        {
                sort_have_separator = true;

                // One byte, or the two that spell a NUL. Anything longer is a
                // separator no line can be split on -- \t among them, which
                // GNU refuses and which a literal tab is the way to ask for.
                bool escaped = said[0] == '\\' && said[1] == '0' && !said[2];

                if (!said[0])
                        return text_refuse(null, "empty tab", 2);

                if (said[1] && !escaped)
                        return text_refuse(said, "multi-character tab", 2);

                sort_separator = escaped ? '\0' : said[0];
        }

        // The global flags are the default for a key that spelled none of
        // its own, and -n after -k on the command line still has to reach
        // the key in front of it. A key that did spell one takes none of
        // them: -r -k1n is a numeric forward key, not a reversed one.
        for (b32 i = 0; i < sort_key_count; i++)
        {
                if (sort_keys[i].ordered)
                        continue;

                sort_keys[i].kind = sort_kind;
                sort_keys[i].reverse = sort_reverse;
                sort_keys[i].how = sort_how;
                sort_keys[i].skip_blanks_first =
                    sort_keys[i].skip_blanks_second = sort_skip_blanks;
        }

        if (null_data)
                text_delimiter = '\0';

        b32 inputs = text_input_count();
        positive address_to run_stop = (positive address_to)text_arena_take(
            ((positive)inputs + 1) * sizeof(positive));

        if (!run_stop)
                return text_done(2);

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_file_name(i)))
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
                string_address name = text_file_name(0);

                if (!name)
                        name = (string_address) "-";

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
                if (!sort_key_count && !sort_kind && !sort_how &&
                    !sort_reverse && !sort_stable && !sort_unique)
                        sort_radix(0, text_lines_count, 0);
                else
                        sort_run(text_lines_count);
        }

        // -o is opened after every line has been read, so sort -o f f still
        // has a file to read.
        if (output)
        {
                bipolar handle = text_open_handle(output, TEXT_WRITE, 0666);

                if (handle < 0)
                        return text_refuse(output,
                                           "cannot open for writing", 2);

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

        return text_done(text_status ? 2 : 0);
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
static text_reader cmp_left;
static text_reader cmp_right;

// Both sides shut and the answer handed back, which is how every exit from
// the comparison below leaves. A shell runs cmp as a builtin and goes on
// living, so a descriptor left open here is a descriptor leaked for good.
static b32 cmp_ends(b32 code)
{
        text_close_handle(address_of cmp_left.opened, cmp_left.handle);
        text_close_handle(address_of cmp_right.opened, cmp_right.handle);

        return text_done(code);
}

static bipolar cmp_byte(text_reader address_to side)
{
        if (!text_reader_fill(side))
                return -1;

        return side->buffer[side->position++];
}

// What is left of a side, which is the whole of it until -i has skipped
// something. Nothing, for a pipe: what is behind one has no length until it
// has ended.
static positive cmp_length(text_reader address_to side)
{
        bipolar here = system_seek(side->handle, 0, FILE_SEEK_CUR);
        bipolar last;

        if (here < 0)
                return 0;

        last = system_seek(side->handle, 0, FILE_SEEK_END);
        system_seek(side->handle, here, FILE_SEEK_SET);

        return last < 0 || last < here ? 0 : (positive)(last - here);
}

// Past a skip, by seeking where that is allowed and by reading where it is
// not. A skip past the end leaves the side empty rather than failing.
static fn cmp_pass(text_reader address_to side, positive count)
{
        if (!count)
                return;

        if (system_seek(side->handle, count, FILE_SEEK_CUR) >= 0)
                return;

        while (count-- && cmp_byte(side) >= 0)
                ;
}

// The line is left out when the differences were listed, because that is
// what the tool this is measured against does.
static fn cmp_ended(text_reader address_to side, positive at, positive line,
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
        positive_into_string(text, at);
        text_error_raw(text);

        if (!listing)
        {
                text_error_raw(", in line ");
                positive_into_string(text, newline ? line : line + 1);
                text_error_raw(text);
        }

        text_error_raw("\n");
}

// A skip or a limit: a count, and one of the suffixes the tool this is
// measured against multiplies it by.
static bool cmp_count_of(string_address value, positive address_to result)
{
        string_address letters = (string_address) "KMGTPEZY";
        positive at = 0;
        positive power = 0;
        positive by = 1024;
        positive total;

        if (!value)
                return false;

        while (byte_is_space(value[at]))
                at++;

        if (value[at] == '+')
                at++;

        positive start = at;
        total = 0;

        while (byte_is_digit(value[at]))
        {
                positive digit = value[at++] - '0';

                if (total > ((positive)-1 - digit) / 10)
                        total = (positive)-1;
                else if (total != (positive)-1)
                        total = total * 10 + digit;
        }

        if (at == start)
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

        if (value[at + 1] == 'i' && value[at + 2] == 'B' && !value[at + 3])
                by = 1024;
        else if (value[at + 1] == 'B' && !value[at + 2])
                by = 1000;
        else if (value[at + 1])
                return false;

        for (positive step = 0; step < power; step++)
                if (total > (positive)-1 / by)
                        total = (positive)-1;
                else
                        total *= by;

        address_to result = total;
        return true;
}

static fn cmp_octal(positive value)
{
        positive_to_base_field(text_put, value, 8, 3, -1, 0);
}

static const file_long cmp_longs[] = {
    {(string_address) "print-bytes", 'b'},
    {(string_address) "ignore-initial", 'i'},
    {(string_address) "bytes", 'n'},
    {(string_address) "quiet", 's'},
    {(string_address) "silent", 's'},
    {(string_address) "verbose", 'l'},
    {null, 0},
};

static b32 text_cmp()
{
        file_taking taking = {
            .program = (string_address) "cmp",
            .allowed = (string_address) "bilns",
            .valued = (string_address) "in",
            .longs = cmp_longs,
        };

        text_begin("cmp");

        if (!file_take(address_of taking))
                return text_done(2);

        bool silent = (taking.flags & FILE_FLAG('s')) != 0;
        bool listing = (taking.flags & FILE_FLAG('l')) != 0;
        bool shown = (taking.flags & FILE_FLAG('b')) != 0;
        b32 index = (b32)taking.first;
        positive at = 0;
        positive lines = 0;
        positive skip_left = 0;
        positive skip_right = 0;
        positive limit = TEXT_UNSET;
        bool newline = true;
        positive width = 1;
        positive scalar_left = 0;
        b32 answer = 0;
        string_address said = file_option_value(address_of taking, 'n');

        if (said && !cmp_count_of(said, address_of limit))
                return text_refuse(said, "invalid --bytes value", 2);

        said = file_option_value(address_of taking, 'i');

        if (said)
        {
                // -i takes one count for both sides or, with a colon between
                // them, one for each.
                positive split = 0;
                p8 head[32];

                while (said[split] && said[split] != ':')
                        split++;

                if (said[split] != ':')
                {
                        if (!cmp_count_of(said, address_of skip_left))
                                return text_refuse(
                                    said, "invalid --ignore-initial value", 2);

                        skip_right = skip_left;
                }
                else
                {
                        if (split >= sizeof(head))
                                split = sizeof(head) - 1;

                        memory_copy_end(head, said, split);

                        if (!cmp_count_of(head, address_of skip_left) ||
                            !cmp_count_of(said + split + 1, address_of skip_right))
                                return text_refuse(
                                    said, "invalid --ignore-initial value", 2);
                }
        }

        b32 operands = text_argument_count - index;

        if (operands < 1 || operands > 4)
                return text_refuse(
                    null, operands ? "extra operand" : "missing operand", 2);

        // The third and fourth operands say the same thing -i does, and say
        // it last, so they win.
        for (b32 which = 2; which < operands; which++)
        {
                positive value = 0;

                if (!cmp_count_of(program_argument(index + which), address_of value))
                        return text_refuse(program_argument(index + which),
                                           "invalid byte count", 2);

                if (which == 2)
                        skip_left = value;
                else
                        skip_right = value;
        }

        // A second name that was not given is standard input, which is how
        // "cmp saved" reads a pipe against a file.
        string_address left_name = program_argument(index);
        string_address right_name = operands > 1 ? program_argument(index + 1)
                                                 : (string_address) "-";

        if (!text_reader_open(address_of cmp_left, left_name))
                return cmp_ends(2);

        if (!text_reader_open(address_of cmp_right, right_name))
                return cmp_ends(2);

        /* One stream cannot be read at two independent positions. GNU cmp
           treats two identical path spellings -- including "-" -- as the
           same object and answers equal without consuming it. */
        if (string_equals(left_name, right_name))
                return cmp_ends(0);

        cmp_pass(address_of cmp_left, skip_left);
        cmp_pass(address_of cmp_right, skip_right);

        if (cmp_left.failed || cmp_right.failed)
                return cmp_ends(2);

        if (listing)
        {
                positive one = cmp_length(address_of cmp_left);
                positive two = cmp_length(address_of cmp_right);
                positive smaller = one < two ? one : two;

                if (limit != TEXT_UNSET && limit < smaller)
                        smaller = limit;

                width = positive_digits(smaller);
        }

        for (;;)
        {
                if (limit != TEXT_UNSET && at >= limit)
                        break;

                /*
                        Equal blocks are cmp's common case.  Prove the whole
                        run in the wide library compare and count its lines in
                        the wide byte counter; only a block containing the
                        first difference falls back to the byte path below.
                */
                if (!scalar_left && text_reader_fill(address_of cmp_left) &&
                    text_reader_fill(address_of cmp_right))
                {
                        positive left = cmp_left.filled - cmp_left.position;
                        positive right = cmp_right.filled - cmp_right.position;
                        positive run = min(left, right);

                        if (limit != TEXT_UNSET && run > limit - at)
                                run = limit - at;

                        p8 address_to one = cmp_left.buffer + cmp_left.position;
                        p8 address_to two = cmp_right.buffer + cmp_right.position;

                        if (run)
                        {
                                bipolar order = memory_compare(one, two, run);

                                if (!order)
                                {
                                        lines += memory_count(one, run, '\n');
                                        newline = one[run - 1] == '\n';
                                        cmp_left.position += run;
                                        cmp_right.position += run;
                                        at += run;
                                        continue;
                                }

                                if (silent)
                                        return cmp_ends(1);

                                if (listing)
                                {
                                        // Do not compare the same known-
                                        // different suffix again per byte.
                                        scalar_left = run;
                                }
                                else
                                {
                                        positive prefix =
                                            memory_common_prefix(one, two, run);

                                        if (prefix)
                                        {
                                                lines += memory_count(one, prefix, '\n');
                                                newline = one[prefix - 1] == '\n';
                                                cmp_left.position += prefix;
                                                cmp_right.position += prefix;
                                                at += prefix;
                                        }
                                }
                        }
                }

                bipolar a = cmp_byte(address_of cmp_left);
                bipolar b = cmp_byte(address_of cmp_right);

                if (cmp_left.failed || cmp_right.failed)
                {
                        answer = 2;
                        break;
                }

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

                if (scalar_left)
                        scalar_left--;

                if (a != b)
                {
                        if (silent)
                                return cmp_ends(1);

                        answer = 1;

                        p8 left[8];
                        p8 right[8];
                        positive wide = 0;

                        if (shown)
                        {
                                wide = text_visible(left, (p8)a);
                                text_visible(right, (p8)b);
                        }

                        if (!listing)
                        {
                                text_put_string(cmp_left.name);
                                text_put_character(' ');
                                text_put_string(cmp_right.name);
                                text_put_string(shown ? " differ: byte "
                                                      : " differ: char ");
                                positive_to_string(text_put, at);
                                text_put_string(", line ");
                                positive_to_string(text_put, lines + 1);

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

                        positive_to_padded(text_put, at, width, ' ', 0);
                        text_put_character(' ');
                        cmp_octal((positive)a);
                        text_put_character(' ');

                        if (shown)
                        {
                                text_put_string(left);

                                // The listing's own column, which is four
                                // wide because M-^? is.
                                writer_fill(text_put, wide < 4 ? 4 - wide : 0, ' ');

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

        return cmp_ends(answer);
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
typedef struct
{
        string_address text;
        bipolar number;
} expr_value;

static string_address expr_empty = (string_address) "";

#define EXPR_ARENA 8192

typedef struct expr_block expr_block;

struct expr_block
{
        expr_block address_to next;
        positive size;
        positive used;
};

static p8 expr_arena[EXPR_ARENA];
static positive expr_arena_used;
static expr_block address_to expr_blocks;
static expr_block address_to expr_here;
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

        if (length == (positive)-1)
        {
                expr_stop("expression too long");
                return expr_empty;
        }

        positive room = length + 1;

        if (room <= EXPR_ARENA - expr_arena_used)
        {
                made = expr_arena + expr_arena_used;
                expr_arena_used += room;
        }
        else
        {
                while (expr_here && room > expr_here->size - expr_here->used)
                        expr_here = expr_here->next;

                if (!expr_here)
                {
                        positive size = room > EXPR_ARENA ? room : EXPR_ARENA;
                        positive got = (positive)memory(sizeof(expr_block) + size);

                        if (!got || got >= (positive)-4095)
                        {
                                expr_stop("expression too long");
                                return expr_empty;
                        }

                        expr_here = (expr_block address_to)got;
                        expr_here->next = null;
                        expr_here->size = size;
                        expr_here->used = 0;

                        if (!expr_blocks)
                                expr_blocks = expr_here;
                        else
                        {
                                expr_block address_to tail = expr_blocks;

                                while (tail->next)
                                        tail = tail->next;

                                tail->next = expr_here;
                        }
                }

                made = (p8 address_to)(expr_here + 1) + expr_here->used;
                expr_here->used += room;
        }

        memory_copy_end(made, from, length);

        return made;
}

static string_address expr_shown(expr_value address_to value)
{
        p8 text[32];
        positive length;

        if (value->text)
                return value->text;

        length = bipolar_into_string(text, value->number);

        value->text = expr_keep(text, length);

        return value->text;
}

// A string is a number only when the whole of it is one; " 1" and "1x" are
// strings, which is why the walk has to reach the terminator.
static bool expr_integer(expr_value address_to value, bipolar address_to out)
{
        if (!value->text)
        {
                address_to out = value->number;
                return true;
        }

        positive taken;
        bipolar made = string_bipolar(value->text, address_of taken);

        if (!taken || string_get(value->text + taken))
                return false;

        address_to out = made;

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
        return expr_at < expr_count ? program_argument(expr_at) : null;
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

        if (!regex_compile(rule, false, false, false, REGEX_POLICY_DEFAULT))
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

        /* A leading plus quotes one otherwise-special operand. This is how
           GNU expr makes `expr + length` mean the literal word "length". */
        if (!string_compare(word, "+"))
        {
                expr_at++;
                word = expr_word();

                if (!word)
                {
                        expr_stop("syntax error");
                        return made;
                }

                expr_at++;
                made.text = word;
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

#define EXPR_LOGICAL_LEVEL(name, lower, spelling, any)                       \
        static expr_value name()                                            \
        {                                                                    \
                expr_value left = lower();                                  \
                                                                             \
                while (!expr_fault && expr_is(spelling))                    \
                {                                                            \
                        bool decided = expr_true(address_of left) == (any);  \
                                                                             \
                        expr_at++;                                           \
                        expr_dead += decided;                                \
                        expr_value right = lower();                          \
                        expr_dead -= decided;                                \
                                                                             \
                        if (expr_fault)                                      \
                                break;                                       \
                        if (decided)                                         \
                        {                                                    \
                                if (!(any))                                  \
                                        left = expr_zero();                  \
                        }                                                    \
                        else if (expr_true(address_of right))                \
                        {                                                    \
                                if (any)                                     \
                                        left = right;                        \
                        }                                                    \
                        else                                                 \
                                left = expr_zero();                          \
                }                                                            \
                                                                             \
                return left;                                                 \
        }

EXPR_LOGICAL_LEVEL(expr_both_level, expr_compare_level, "&", false)
EXPR_LOGICAL_LEVEL(expr_any, expr_both_level, "|", true)
#undef EXPR_LOGICAL_LEVEL

static b32 text_expr()
{
        expr_value result;

        text_begin("expr");

        expr_at = 1;
        expr_count = text_argument_count;
        expr_arena_used = 0;
        expr_here = expr_blocks;

        for (expr_block address_to block = expr_blocks; block; block = block->next)
                block->used = 0;

        expr_fault = 0;
        expr_dead = 0;

        if (expr_at < expr_count && !string_compare(program_argument(expr_at), "--"))
                expr_at++;

        if (expr_at >= expr_count)
                return text_refuse(null, "missing operand", 2);

        result = expr_any();

        if (!expr_fault && expr_at < expr_count)
                expr_stop("syntax error");

        if (expr_fault)
                return text_done(2);

        text_put_string(expr_shown(address_of result));
        text_put_character('\n');

        return text_done(expr_true(address_of result) ? 0 : 1);
}
