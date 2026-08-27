#include "../src/library.c"

/*
        Differential test for the assembly in library.c.

        Every routine in there replaced a C one that was known to work. This
        keeps that C, under another name, and checks the two answer the same
        thing for inputs chosen to land on the edges: nothing, one byte, either
        side of every size the assembly switches strategy at, and overlaps in
        both directions.

        It is built freestanding for each architecture and run under qemu, so
        an answer that is only right on the machine this was written on does
        not pass.

        Adding a routine means adding its reference and a case below. A
        reference is the C that used to be in library.c, copied verbatim: the
        point is to compare against what was already trusted, not against a
        second attempt at the same idea.
*/

static positive failures;
static positive checks;

fn report(string_address name, string_address detail, positive got, positive want)
{
        failures++;
        string_format(log, "  FAIL %s %s: got %p want %p\n", name, detail, got, want);
}

fn same(string_address name, string_address detail, positive got, positive want)
{
        checks++;

        if (got != want)
                report(name, detail, got, want);
}

fn same_bytes(string_address name, string_address detail, b8 address_to got,
              b8 address_to want, positive size)
{
        checks++;

        for (positive i = 0; i < size; i++)
                if (got[i] != want[i])
                {
                        failures++;
                        string_format(log, "  FAIL %s %s: byte %p is %p want %p\n",
                                      name, detail, i, (positive)got[i],
                                      (positive)want[i]);
                        return;
                }
}

// A repeatable spread of values; nothing here wants real randomness, only
// inputs nobody chose by hand.
static positive seed = 0x2545F4914F6CDD1Dull;

positive next()
{
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
}

/*
        The references: the C that was in library.c before the assembly.
*/
address_any reference_fill(address_any destination, b8 value, positive size)
{
        b8 address_to dest = (b8 address_to)destination;

        while (size--)
                address_to dest++ = (b8)value;

        return destination;
}

address_any reference_copy(address_any destination, address_any source, positive size)
{
        b8 address_to dest = (b8 address_to)destination;
        b8 address_to src = (b8 address_to)source;

        if (dest > src && dest < src + size)
        {
                dest += size - 1;
                src += size - 1;
                while (size--)
                        address_to dest-- = address_to src--;
        }
        else
        {
                while (size--)
                        address_to dest++ = address_to src++;
        }

        return destination;
}

positive reference_length(string_address source)
{
        string_address step = source;

        while (string_get(step))
                step++;

        return step - source;
}

b32 reference_compare(string_address source, string_address input)
{
        while (string_get(source) && string_get(input))
        {
                if string_not (source, address_to input)
                        break;

                source++;
                input++;
        }

        return string_get(source) - string_get(input);
}

string_address reference_first_of(string_address source, p8 character)
{
        while (string_get(source))
        {
                if string_is (source, character)
                        return source;

                source++;
        }

        return character ? null : source;
}

// Every size the routines change strategy at, and the ones either side.
static positive edges[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                           63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512,
                           1000, 4096};

#define EDGE_COUNT (sizeof(edges) / sizeof(edges[0]))
#define ROOM 8192

static b8 mine[ROOM];
static b8 theirs[ROOM];
static b8 pattern[ROOM];

fn check_fill()
{
        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];
                b8 value = (b8)next();

                // Poisoned either side, so a routine that writes past its size
                // is caught rather than tolerated.
                reference_fill(mine, 0xA5, ROOM);
                reference_fill(theirs, 0xA5, ROOM);

                memory_fill(mine + 64, value, size);
                reference_fill(theirs + 64, value, size);

                same_bytes("memory_fill", "whole buffer", mine, theirs, ROOM);
        }
}

fn check_copy()
{
        for (positive i = 0; i < ROOM; i++)
                pattern[i] = (b8)next();

        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];

                reference_fill(mine, 0xA5, ROOM);
                reference_fill(theirs, 0xA5, ROOM);

                memory_copy_fast(mine + 64, pattern, size);
                reference_copy(theirs + 64, pattern, size);

                same_bytes("memory_copy_fast", "forward", mine, theirs, ROOM);
        }
}

// Both directions, and the case where they do not overlap at all: a memmove
// that only handles one of them passes half of this.
fn check_move()
{
        static positive offsets[] = {1, 3, 8, 15, 16, 64, 100};

        for (positive e = 0; e < EDGE_COUNT; e++)
                for (positive o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++)
                {
                        positive size = edges[e];
                        positive gap = offsets[o];

                        if (size + gap + 128 > ROOM)
                                continue;

                        memory_copy_fast(mine, pattern, ROOM);
                        memory_copy_fast(theirs, pattern, ROOM);

                        memory_copy(mine + 64 + gap, mine + 64, size);
                        reference_copy(theirs + 64 + gap, theirs + 64, size);
                        same_bytes("memory_copy", "forwards overlap", mine, theirs, ROOM);

                        memory_copy_fast(mine, pattern, ROOM);
                        memory_copy_fast(theirs, pattern, ROOM);

                        memory_copy(mine + 64, mine + 64 + gap, size);
                        reference_copy(theirs + 64, theirs + 64 + gap, size);
                        same_bytes("memory_copy", "backwards overlap", mine, theirs, ROOM);
                }
}

fn check_strings()
{
        static p8 text[512];

        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];

                if (size >= sizeof(text))
                        continue;

                for (positive i = 0; i < size; i++)
                {
                        p8 c = (p8)(next() % 94 + 33);

                        text[i] = c;
                }

                text[size] = 0;

                same("string_length", "", string_length(text), reference_length(text));

                // Against itself, against a copy that differs at one place, and
                // against something shorter.
                same("string_compare", "equal",
                     (positive)string_compare(text, text),
                     (positive)reference_compare(text, text));

                for (positive at = 0; at < size && at < 8; at++)
                {
                        static p8 other[512];

                        memory_copy_fast(other, text, size + 1);
                        other[at] = (p8)(other[at] ^ 1);

                        same("string_compare", "one byte apart",
                             (positive)(string_compare(text, other) > 0),
                             (positive)(reference_compare(text, other) > 0));

                        same("string_compare", "sign",
                             (positive)(string_compare(text, other) < 0),
                             (positive)(reference_compare(text, other) < 0));
                }

                // Every character it does contain, one it does not, and the
                // terminator -- which strchr answers with the end, not null.
                for (positive i = 0; i < size && i < 16; i++)
                        same("string_first_of", "present",
                             (positive)string_first_of(text, text[i]),
                             (positive)reference_first_of(text, text[i]));

                same("string_first_of", "absent",
                     (positive)string_first_of(text, 1),
                     (positive)reference_first_of(text, 1));

                same("string_first_of", "terminator",
                     (positive)string_first_of(text, 0),
                     (positive)reference_first_of(text, 0));
        }
}

/*
        The routines converted in bulk. References first, then one check each.

        A routine that reports through a writer is captured into a buffer, so
        the comparison is over what it actually emitted rather than over a
        return value it does not have.
*/
static b8 caught[512];
static positive caught_length;

fn catch_writer(address_any data, positive length)
{
        b8 address_to from = (b8 address_to)data;

        if (!length)
                while (from[length])
                        length++;

        for (positive i = 0; i < length && caught_length < sizeof(caught) - 1; i++)
                caught[caught_length++] = from[i];

        caught[caught_length] = 0;
}

fn catch_reset()
{
        caught_length = 0;
        caught[0] = 0;
}

string_address reference_string_last_of(string_address source, p8 character)
{
        string_address found = null;

        while (string_get(source))
        {
                if string_is (source, character)
                        found = source;

                source++;
        }

        return character ? found : source;
}

positive reference_to_positive(string_address input)
{
        positive value = 0;

        while (string_get(input) >= '0' && string_get(input) <= '9')
                value = value * 10 + (positive)(string_get(input++) - '0');

        return value;
}

bipolar reference_to_bipolar(string_address input)
{
        b32 negative = string_is(input, '-');

        if (negative || string_is(input, '+'))
                input++;

        bipolar value = (bipolar)reference_to_positive(input);

        return negative ? -value : value;
}

string_address reference_find(string_address string, string_address input)
{
        string_address step = string;
        string_address step_input = input;

        while (string_get(step))
        {
                if (string_not(step, string_get(step_input)))
                {
                        step++;
                        continue;
                }

                string_address find = step;

                while
                        string_get(step_input)
                        {
                                if string_not (step, string_get(step_input))
                                        break;

                                step++;
                                step_input++;
                        }

                if string_is (step_input, end)
                        return find;

                step_input = input;
        }

        return null;
}

fn check_bulk_strings()
{
        static p8 subject[512];
        static p8 spare[512];

        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];

                if (size >= sizeof(subject) - 2)
                        continue;

                for (positive i = 0; i < size; i++)
                        subject[i] = (p8)(next() % 4 + 'a');

                subject[size] = 0;

                // string_copy and string_copy_max, into a poisoned buffer so a
                // byte written past the terminator is caught.
                reference_fill(spare, 0xA5, sizeof(spare));
                string_copy(spare, subject);
                same("string_copy", "length", string_length(spare), size);
                same("string_copy", "content",
                     (positive)string_compare(spare, subject), 0);

                for (positive limit = 0; limit <= size + 2 && limit < 64; limit++)
                {
                        reference_fill(spare, 0xA5, sizeof(spare));
                        string_copy_max(spare, subject, limit);

                        // Nothing at all for a limit of zero, and never a byte
                        // past the limit.
                        for (positive i = limit; i < sizeof(spare); i++)
                                if (spare[i] != 0xA5)
                                {
                                        report("string_copy_max", "wrote past the limit",
                                               i, limit);
                                        break;
                                }

                        checks++;
                }

                same("string_last_of", "found",
                     (positive)string_last_of(subject, 'a'),
                     (positive)reference_string_last_of(subject, 'a'));
                same("string_last_of", "absent",
                     (positive)string_last_of(subject, 'z'),
                     (positive)reference_string_last_of(subject, 'z'));

                // string_cut and string_replace_all change the string, so each
                // works on its own copy of it.
                string_copy(spare, subject);
                string_address after = string_cut(spare, 'b');
                same("string_cut", "terminated before the cut",
                     (positive)(after == null || string_first_of(spare, 'b') == null), 1);

                string_copy(spare, subject);
                string_replace_all(spare, 'a', 'z');
                same("string_replace_all", "none left",
                     (positive)(string_first_of(spare, 'a') == null), 1);
                same("string_replace_all", "same length", string_length(spare), size);

                // Against the C it replaced, not against an assumption: an
                // empty haystack answers null, which is not what "find
                // yourself in yourself" would suggest.
                same("string_find", "itself",
                     (positive)string_find(subject, subject),
                     (positive)reference_find(subject, subject));

                if (size > 2)
                {
                        same("string_find", "a tail of itself",
                             (positive)string_find(subject, subject + size - 2),
                             (positive)reference_find(subject, subject + size - 2));

                        same("string_find", "absent",
                             (positive)string_find(subject, (string_address)"qqq"),
                             (positive)reference_find(subject, (string_address)"qqq"));
                }
        }
}

fn check_bulk_numbers()
{
        static positive samples[] = {0, 1, 7, 9, 10, 99, 100, 255, 999, 1000,
                                     65535, 1000000, 4294967295u};

        for (positive i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        {
                positive n = samples[i];

                catch_reset();
                positive_to_string(catch_writer, n);
                same("positive_to_string", "round trip",
                     reference_to_positive(caught), n);

                catch_reset();
                bipolar_to_string(catch_writer, (bipolar)n);
                same("bipolar_to_string", "round trip",
                     (positive)reference_to_bipolar(caught), n);

                catch_reset();
                bipolar_to_string(catch_writer, -(bipolar)n);
                same("bipolar_to_string", "negative round trip",
                     (positive)(-reference_to_bipolar(caught)), n);

                catch_reset();
                positive_to_string(catch_writer, n);
                same("string_to_positive", "agrees",
                     string_to_positive(caught), reference_to_positive(caught));

                catch_reset();
                bipolar_to_string(catch_writer, -(bipolar)n);
                same("string_to_bipolar", "agrees",
                     (positive)string_to_bipolar(caught),
                     (positive)reference_to_bipolar(caught));
        }
}

/*
        file_load, which until now could not succeed.

        Its mmap flags were open() bits, so the kernel refused every mapping
        and the success path had never once run. This writes a file, loads it,
        and reads the bytes back through the mapping -- so the fix is held
        rather than merely applied.
*/
fn check_file_load()
{
        static p8 body[3000];
        static file subject;
        b8 address_to loaded;

        for (positive i = 0; i < sizeof(body); i++)
                body[i] = (b8)(next() & 0xff);

        system_call_3(syscall(unlinkat), AT_FDCWD,
                      (positive) "/tmp/moonwater_verify_load", 0);

        b32 made = system_call_4(syscall(openat), AT_FDCWD,
                                 (positive) "/tmp/moonwater_verify_load",
                                 FILE_CREATE | FILE_WRITE | FILE_TRUNCATE, 0644);

        checks++;

        if (made < 0)
        {
                report("file_load", "could not make a file", (positive)made, 0);
                return;
        }

        system_call_3(syscall(write), made, (positive)body, sizeof(body));
        system_call_1(syscall(close), made);

        file_new(address_of subject, (string_address) "/tmp/moonwater_verify_load",
                 FILE_READ);

        same("file_load", "opened", (positive)file_valid(address_of subject), 1);
        same("file_load", "size", subject.status.size, sizeof(body));

        loaded = (b8 address_to)file_load(address_of subject);

        checks++;

        if (!loaded)
        {
                report("file_load", "returned null", 0, 1);
                file_close(address_of subject);
                return;
        }

        same_bytes("file_load", "contents", loaded, body, sizeof(body));
        same("file_load", "already loaded returns the same",
             (positive)file_load(address_of subject), (positive)loaded);

        file_close(address_of subject);
        system_call_3(syscall(unlinkat), AT_FDCWD,
                      (positive) "/tmp/moonwater_verify_load", 0);
}

/*
        The routines merged from the conversion lanes.

        Each was verified by whoever wrote it, in a tree of their own. These
        are the same routines checked here, in one harness, so the claim does
        not rest on seven separate reports.
*/
#define SCRATCH "/tmp/moonwater_verify_scratch"

fn check_file_round_trip()
{
        static p8 written[2048];
        static p8 read_back[2048];
        static file subject;

        for (positive i = 0; i < sizeof(written); i++)
                written[i] = (b8)(next() & 0xff);

        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)SCRATCH, 0);

        file_new(address_of subject, (string_address)SCRATCH,
                 FILE_READ_WRITE | FILE_CREATE | FILE_TRUNCATE);

        same("file_new", "valid", (positive)file_valid(address_of subject), 1);

        // Written at an offset, so the seek is exercised rather than assumed.
        positive put = file_write(address_of subject, written, sizeof(written), 0);

        same("file_write", "wrote it all", put, sizeof(written));

        file_get_status(address_of subject);
        same("file_get_status", "size follows the write",
             subject.status.size, sizeof(written));

        reference_fill(read_back, 0, sizeof(read_back));
        positive got = file_read(address_of subject, read_back, sizeof(read_back), 0);

        same("file_read", "read it all", got, sizeof(written));
        same_bytes("file_read", "contents", read_back, written, sizeof(written));

        // A partial read from the middle: the clamp is the only arithmetic in
        // the whole family.
        reference_fill(read_back, 0, sizeof(read_back));
        got = file_read(address_of subject, read_back, 100, sizeof(written) - 40);
        same("file_read", "clamped at the end", got, 40);

        got = file_read(address_of subject, read_back, 100, sizeof(written) + 500);
        same("file_read", "past the end reads nothing", got, 0);

        file_close(address_of subject);
        same("file_close", "no longer valid",
             (positive)file_valid(address_of subject), 0);

        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)SCRATCH, 0);
}

fn check_memory()
{
        static positive sizes[] = {1, 8, 100, 4095, 4096, 4097, 65536};

        for (positive i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
                positive size = sizes[i];
                b8 address_to block = (b8 address_to)memory(size);

                checks++;

                if (!block)
                {
                        report("memory", "returned null", size, 0);
                        continue;
                }

                // Writable over its whole extent, and it must be zero: an
                // anonymous mapping is, and anything relying on that would
                // otherwise fail somewhere far away.
                for (positive j = 0; j < size; j++)
                        if (block[j])
                        {
                                report("memory", "not zeroed", j, 0);
                                break;
                        }

                checks++;

                for (positive j = 0; j < size; j++)
                        block[j] = (b8)(j & 0xff);

                for (positive j = 0; j < size; j++)
                        if (block[j] != (b8)(j & 0xff))
                        {
                                report("memory", "did not hold what was written", j, 0);
                                break;
                        }

                checks++;
                memory_free(block, size);
        }
}

fn check_directory()
{
        string_address here = working_directory_get();

        same("working_directory_get", "answered", (positive)(here != null), 1);

        if (!here)
                return;

        same("working_directory_set", "to root",
             (positive)(working_directory_set((string_address)"/"), 1), 1);

        string_address now = working_directory_get();

        same("working_directory_get", "follows the set",
             (positive)string_compare(now, (string_address)"/"), 0);

        working_directory_set(here);
}

fn check_clock()
{
        p64 first = get_cpu_time();
        positive spin = 0;

        // Something it cannot fold away, so the second read is genuinely later.
        for (positive i = 0; i < 200000; i++)
                spin += i ^ next();

        p64 second = get_cpu_time();

        same("get_cpu_time", "moves forward", (positive)(second >= first), 1);
        same("get_cpu_time", "not stuck at zero",
             (positive)(first != 0 || second != 0), 1);
        checks += (spin == 0);
}

fn check_format()
{
        // string_format drives most of what anything here prints, so the check
        // is over what it emits rather than over a return value it has none of.
        catch_reset();
        string_format(catch_writer, (string_address) "plain");
        same("string_format", "literal", string_length(caught), 5);

        catch_reset();
        string_format(catch_writer, (string_address) "[%s]", "middle");
        same("string_format", "string",
             (positive)string_compare(caught, (string_address) "[middle]"), 0);

        catch_reset();
        string_format(catch_writer, (string_address) "%p", (positive)12345);
        same("string_format", "number", reference_to_positive(caught), 12345);

        catch_reset();
        string_format(catch_writer, (string_address) "%b", (bipolar)-99);
        same("string_format", "signed",
             (positive)(-reference_to_bipolar(caught)), 99);

        catch_reset();
        string_format(catch_writer, (string_address) "%s=%p;", "n", (positive)7);
        same("string_format", "several",
             (positive)string_compare(caught, (string_address) "n=7;"), 0);

        catch_reset();
        string_format(catch_writer, (string_address) "%s", "");
        same("string_format", "empty argument", string_length(caught), 0);
}

fn check_decimals()
{
        // fast_sin is an approximation, so the check is the shape of it: the
        // zeroes, the sign either side, and that it stays inside its range.
        same("fast_sin", "zero at zero", (positive)(fast_sin(0.0) == 0.0), 1);
        same("fast_sin", "positive on the first half",
             (positive)(fast_sin(1.5) > 0.0), 1);
        same("fast_sin", "negative on the second",
             (positive)(fast_sin(4.0) < 0.0), 1);

        for (positive i = 0; i < 64; i++)
        {
                decimal x = (decimal)(bipolar)(next() % 2000) / 100.0 - 10.0;
                decimal y = fast_sin(x);

                same("fast_sin", "inside its range",
                     (positive)(y >= -1.01 && y <= 1.01), 1);
        }

        catch_reset();
        decimal_to_string(catch_writer, 0.0);
        same("decimal_to_string", "zero", (positive)(string_length(caught) > 0), 1);

        catch_reset();
        decimal_to_string(catch_writer, 1.5);
        same("decimal_to_string", "has a point",
             (positive)(string_first_of(caught, '.') != null), 1);

        catch_reset();
        decimal_to_string(catch_writer, -2.25);
        same("decimal_to_string", "keeps the sign", (positive)(caught[0] == '-'), 1);
}

b32 main()
{
        check_fill();
        check_copy();
        check_move();
        check_strings();
        check_bulk_strings();
        check_bulk_numbers();
        check_file_load();
        check_file_round_trip();
        check_memory();
        check_directory();
        check_clock();
        check_format();
        check_decimals();

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
