#include "../compiler_memory.c"

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

#include "counted.inc"

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

static positive absent_count;

// A routine the library does not carry under either of its names.
//
// Not a failure: the inventory says which machines are short of which, and
// riscv64 is legitimately short of several. Saying so out loud is the point --
// a case that compiles to nothing says nothing, and that is how ninety five
// thousand checks once went missing while the lane still agreed with itself.
fn absent(string_address name)
{
        absent_count++;
        string_format(log, "  ABSENT %s: under neither name in this library\n", name);
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

fn reference_exchange(address_any left, address_any right, positive size)
{
        p8 address_to a = left;
        p8 address_to b = right;

        if (a == b)
                return;

        while (size--)
        {
                p8 held = *a;

                *a++ = *b;
                *b++ = held;
        }
}

address_any reference_reverse(address_any block, positive size)
{
        p8 address_to left = block;
        p8 address_to right = left + size;

        while (left < right)
        {
                p8 value;

                right--;

                if (left >= right)
                        break;

                value = address_to left;
                address_to left++ = address_to right;
                address_to right = value;
        }

        return block;
}

address_any reference_frob(address_any block, positive size)
{
        p8 address_to bytes = block;

        while (size--)
                address_to bytes++ ^= 42;

        return block;
}

p8 address_to reference_copy_end(p8 address_to destination, address_any source,
                                 positive size)
{
        reference_copy(destination, source, size);
        destination[size] = 0;
        return destination + size;
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

/*
        The two wide fills, which write a repeated four or eight byte pattern
        rather than a repeated byte.

        Their count is in ELEMENTS and not in bytes, which is the one thing a
        caller gets wrong about them, so the buffer is poisoned either side
        and compared whole: a routine that reads its count as bytes writes a
        quarter or an eighth of what it should, and the poison says so rather
        than the comparison passing on the part that was written.
*/
fn check_fill_wide()
{
        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive count = edges[e] / 8;
                p32 narrow = (p32)next() | 1u;
                positive wide = ((positive)next() << 32) | (positive)next() | 1u;
                positive at;

                reference_fill(mine, 0xA5, ROOM);
                reference_fill(theirs, 0xA5, ROOM);
                memory_fill_32(mine + 64, narrow, count);
                for (at = 0; at < count; at++)
                        reference_copy(theirs + 64 + at * 4, address_of narrow, 4);
                same_bytes("memory_fill_32", "whole buffer", mine, theirs, ROOM);

                reference_fill(mine, 0xA5, ROOM);
                reference_fill(theirs, 0xA5, ROOM);
                memory_fill_64(mine + 64, wide, count);
                for (at = 0; at < count; at++)
                        reference_copy(theirs + 64 + at * 8, address_of wide, 8);
                same_bytes("memory_fill_64", "whole buffer", mine, theirs, ROOM);
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

                memory_copy_apart(mine + 64, pattern, size);
                reference_copy(theirs + 64, pattern, size);

                same_bytes("memory_copy_apart", "forward", mine, theirs, ROOM);
        }
}

/*
        The qsort floor exchanges disjoint records, but neither record has an
        alignment promise. Every short width is crossed with every pair of
        residues, and poison around both records catches a load or store on
        either side. The larger widths cross the vector/unrolled loop edges.
        Equal addresses and zero bytes are contractual no-ops and are checked
        without requiring either address to be readable.
*/
#define EXCHANGE_ROOM 4608

static p8 exchange_got[EXCHANGE_ROOM];
static p8 exchange_want[EXCHANGE_ROOM];

static fn exchange_case(positive left, positive right, positive size,
                        string_address detail)
{
        positive extent = right + size + 32;

        reference_fill(exchange_got, 0xa5, extent);
        reference_fill(exchange_want, 0xa5, extent);

        for (positive i = 0; i < size; i++)
        {
                p8 a = (p8)next();
                p8 b = (p8)next();

                exchange_got[left + i] = exchange_want[left + i] = a;
                exchange_got[right + i] = exchange_want[right + i] = b;
        }

        memory_exchange_apart(exchange_got + left, exchange_got + right, size);
        reference_exchange(exchange_want + left, exchange_want + right, size);
        same_bytes("memory_exchange_apart", detail, exchange_got, exchange_want,
                   extent);
}

fn check_exchange()
{
        static const positive large_widths[] = {
            129, 255, 256, 257, 511, 512, 513, 1000, 2048,
        };

        memory_exchange_apart(null, null, 0);
        memory_exchange_apart(null, null, 37);

        reference_fill(exchange_got, 0xa5, EXCHANGE_ROOM);
        reference_fill(exchange_want, 0xa5, EXCHANGE_ROOM);
        memory_exchange_apart(exchange_got + 31, exchange_got + 31, 257);
        same_bytes("memory_exchange_apart", "equal address is untouched",
                   exchange_got, exchange_want, EXCHANGE_ROOM);

        for (positive size = 0; size <= 128; size++)
                for (positive left_residue = 0; left_residue < 16; left_residue++)
                        for (positive right_residue = 0; right_residue < 16;
                             right_residue++)
                                exchange_case(32 + left_residue,
                                              256 + right_residue, size,
                                              "every short width and residue");

        for (positive width = 0;
             width < sizeof(large_widths) / sizeof(large_widths[0]); width++)
                for (positive left_residue = 0; left_residue < 16; left_residue++)
                        for (positive right_residue = 0; right_residue < 16;
                             right_residue++)
                                exchange_case(64 + left_residue,
                                              2304 + right_residue,
                                              large_widths[width],
                                              "bulk loop edges and residues");
}

/*
        Cross every strategy boundary with every residue through a vector,
        while poison on both sides proves the exact span is the only span
        written. The second turn checks the defining involution too.
*/
fn check_frob()
{
        same("memory_frob", "null zero-sized return",
             (positive)memory_frob(null, 0), 0);

        for (positive e = 0; e < EDGE_COUNT; e++)
                for (positive residue = 0; residue < 32; residue++)
                {
                        positive size = edges[e];
                        positive offset = 64 + residue;

                        reference_fill(mine, 0xa5, ROOM);
                        reference_fill(theirs, 0xa5, ROOM);

                        for (positive i = 0; i < size; i++)
                        {
                                p8 value = (p8)next();

                                mine[offset + i] = theirs[offset + i] = value;
                        }

                        same("memory_frob", "returned original address",
                             (positive)memory_frob(mine + offset, size),
                             (positive)(address_any)(mine + offset));
                        reference_frob(theirs + offset, size);
                        same_bytes("memory_frob", "size, residue, and guards",
                                   mine, theirs, ROOM);

                        memory_frob(mine + offset, size);
                        reference_frob(theirs + offset, size);
                        same_bytes("memory_frob", "second turn restores bytes",
                                   mine, theirs, ROOM);
                }
}

/*
        Exact in-place reversal, not string reversal: zero bytes in the span
        are ordinary data and there is no terminator to find.  Every short
        size is crossed with every residue through thirty one, poison proves
        both guards remain untouched, and a second turn proves the operation
        is its own inverse.  The random, page-edge and one-megabyte cases keep
        the small exhaustive matrix from being the only shape exercised.
*/
#define REVERSE_MEDIUM (1 << 15)
#define REVERSE_LARGE ((1 << 20) + 128)

static p8 reverse_got[REVERSE_MEDIUM];
static p8 reverse_want[REVERSE_MEDIUM];
static p8 reverse_large_got[REVERSE_LARGE];
static p8 reverse_large_want[REVERSE_LARGE];

static fn reverse_prepare(positive extent, positive offset, positive size)
{
        reference_fill(reverse_got, 0xa5, extent);
        reference_fill(reverse_want, 0xa5, extent);

        for (positive i = 0; i < size; i++)
                reverse_got[offset + i] = reverse_want[offset + i] = (p8)next();
}

fn check_reverse()
{
        same("memory_reverse", "null zero-sized return",
             (positive)memory_reverse(null, 0), 0);

        for (positive size = 0; size <= 512; size++)
                for (positive residue = 0; residue < 32; residue++)
                {
                        positive offset = 64 + residue;
                        positive extent = offset + size + 64;
                        p8 address_to got = reverse_got + offset;
                        p8 address_to want = reverse_want + offset;

                        reverse_prepare(extent, offset, size);

                        same("memory_reverse", "returned original address",
                             (positive)memory_reverse(got, size),
                             (positive)(address_any)got);
                        reference_reverse(want, size);
                        same_bytes("memory_reverse", "all small sizes and residues",
                                   reverse_got, reverse_want, extent);

                        memory_reverse(got, size);
                        reference_reverse(want, size);
                        same_bytes("memory_reverse", "double reverse",
                                   reverse_got, reverse_want, extent);
                }

        // Repeatable random lengths and contents, with an independently
        // changing residue and guard width on every turn.
        for (positive turn = 0; turn < 512; turn++)
        {
                positive size = next() % 16385;
                positive offset = 33 + (next() & 63);
                positive extent = offset + size + 71;

                reverse_prepare(extent, offset, size);
                memory_reverse(reverse_got + offset, size);
                reference_reverse(reverse_want + offset, size);
                same_bytes("memory_reverse", "random length and contents",
                           reverse_got, reverse_want, extent);
        }

        // Put each exclusive end exactly on a 4096-byte boundary.  The
        // arrays keep mapped poison on both sides; exact-access floor audits
        // separately prove the implementations do not issue a wider access.
        {
                static const positive page_sizes[] = {
                    0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                    63, 64, 65, 255, 256, 257, 4095, 4096, 4097,
                };
                positive base = (positive)(address_any)reverse_got;
                positive edge_offset = ((base + 4095) & ~(positive)4095) - base +
                                       8192;

                for (positive c = 0;
                     c < sizeof(page_sizes) / sizeof(page_sizes[0]); c++)
                {
                        positive size = page_sizes[c];
                        positive offset = edge_offset - size;
                        positive extent = edge_offset + 64;

                        reverse_prepare(extent, offset, size);
                        memory_reverse(reverse_got + offset, size);
                        reference_reverse(reverse_want + offset, size);
                        same_bytes("memory_reverse", "exclusive end at page edge",
                                   reverse_got, reverse_want, extent);
                }
        }

        // A resident one-megabyte span makes the bulk loop do real work and
        // catches counters or pointer arithmetic accidentally narrowed to 32
        // bits in a way the short matrix cannot.
        for (positive i = 0; i < REVERSE_LARGE; i++)
        {
                p8 value = (p8)next();

                reverse_large_got[i] = reverse_large_want[i] = value;
        }

        memory_reverse(reverse_large_got + 37, 1 << 20);
        reference_reverse(reverse_large_want + 37, 1 << 20);
        same_bytes("memory_reverse", "one megabyte with guards",
                   reverse_large_got, reverse_large_want, REVERSE_LARGE);

        memory_reverse(reverse_large_got + 37, 1 << 20);
        reference_reverse(reverse_large_want + 37, 1 << 20);
        same_bytes("memory_reverse", "one megabyte double reverse",
                   reverse_large_got, reverse_large_want, REVERSE_LARGE);
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

                        memory_copy_apart(mine, pattern, ROOM);
                        memory_copy_apart(theirs, pattern, ROOM);

                        memory_copy(mine + 64 + gap, mine + 64, size);
                        reference_copy(theirs + 64 + gap, theirs + 64, size);
                        same_bytes("memory_copy", "forwards overlap", mine, theirs, ROOM);

                        memory_copy_apart(mine, pattern, ROOM);
                        memory_copy_apart(theirs, pattern, ROOM);

                        memory_copy(mine + 64, mine + 64 + gap, size);
                        reference_copy(theirs + 64, theirs + 64 + gap, size);
                        same_bytes("memory_copy", "backwards overlap", mine, theirs, ROOM);
                }
}

/*
        Exact bytes followed by a terminator through the non-overlap core.

        Source and destination are separate arrays by contract. Every source
        and destination alignment is crossed with every copy-size edge, and
        poison across the whole destination proves that only dst[0..size]
        moved. Embedded zeroes prove this is a memory primitive, while the
        returned address proves the adapter preserved dst+n across its call.
*/
fn check_copy_fast_end()
{
        for (positive i = 0; i < ROOM; i++)
                pattern[i] = (i % 11) ? (b8)(i * 37 + 5) : 0;

        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];

                for (positive destination_offset = 0; destination_offset < 16;
                     destination_offset++)
                        for (positive source_offset = 0; source_offset < 16;
                             source_offset++)
                        {
                                positive destination = 64 + destination_offset;
                                p8 address_to ended;

                                if (destination + size + 2 >= ROOM ||
                                    source_offset + size >= ROOM)
                                        continue;

                                reference_fill(mine, 0xA5, ROOM);
                                reference_fill(theirs, 0xA5, ROOM);

                                ended = memory_copy_apart_end(
                                    (p8 address_to)mine + destination,
                                    pattern + source_offset, size);
                                reference_copy_end(
                                    (p8 address_to)theirs + destination,
                                    pattern + source_offset, size);

                                same("memory_copy_apart_end", "returned unaligned end",
                                     (positive)(ended - (p8 address_to)mine),
                                     destination + size);
                                same_bytes("memory_copy_apart_end", "guarded exact copy",
                                           mine, theirs, ROOM);
                        }
        }

        // A zero-byte copy writes only the terminator and never reads source.
        reference_fill(mine, 0xA5, ROOM);
        reference_fill(theirs, 0xA5, ROOM);
        same("memory_copy_apart_end", "zero length end",
             (positive)(memory_copy_apart_end((p8 address_to)mine + 31, null, 0) -
                        (p8 address_to)mine),
             31);
        theirs[31] = 0;
        same_bytes("memory_copy_apart_end", "zero length guard", mine, theirs, ROOM);
}

/*
        Exact bytes followed by a terminator, through the overlap-aware core.

        The source deliberately contains embedded zeroes: this is a memory
        primitive and string_copy_max_end would stop early. Poison around every
        destination catches a write on either side of dst[0..size], while the
        returned address says the wrapper kept the original end across the
        shared copy core.
*/
fn check_copy_end()
{
        static b32 gaps[] = {-65, -32, -9, -3, -1, 1, 3, 9, 32, 65};

        for (positive i = 0; i < ROOM; i++)
                pattern[i] = (i % 11) ? (b8)(i * 37 + 5) : 0;

        // Disjoint source and destination, at every offset into a machine word.
        for (positive e = 0; e < EDGE_COUNT; e++)
        {
                positive size = edges[e];

                for (positive destination_offset = 0; destination_offset < 8;
                     destination_offset++)
                        for (positive source_offset = 0; source_offset < 8;
                             source_offset++)
                        {
                                positive destination = 64 + destination_offset;
                                p8 address_to ended;

                                if (destination + size + 2 >= ROOM ||
                                    source_offset + size >= ROOM)
                                        continue;

                                reference_fill(mine, 0xA5, ROOM);
                                reference_fill(theirs, 0xA5, ROOM);

                                ended = memory_copy_end(
                                    (p8 address_to)mine + destination,
                                    pattern + source_offset, size);
                                reference_copy_end(
                                    (p8 address_to)theirs + destination,
                                    pattern + source_offset, size);

                                same("memory_copy_end", "returned aligned end",
                                     (positive)(ended - (p8 address_to)mine),
                                     destination + size);
                                same_bytes("memory_copy_end", "guarded exact copy",
                                           mine, theirs, ROOM);
                        }
        }

        // Both overlap directions and distances inside and outside the core's
        // head/tail chunks, again at every word alignment.
        for (positive e = 0; e < EDGE_COUNT; e++)
                for (positive alignment = 0; alignment < 8; alignment++)
                        for (positive g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
                        {
                                positive size = edges[e];
                                b32 source = 1024 + (b32)alignment;
                                b32 destination = source + gaps[g];
                                p8 address_to ended;

                                if (destination < 0 ||
                                    (positive)destination + size + 2 >= ROOM ||
                                    (positive)source + size >= ROOM)
                                        continue;

                                reference_copy(mine, pattern, ROOM);
                                reference_copy(theirs, pattern, ROOM);

                                ended = memory_copy_end(
                                    (p8 address_to)mine + destination,
                                    mine + source, size);
                                reference_copy_end(
                                    (p8 address_to)theirs + destination,
                                    theirs + source, size);

                                same("memory_copy_end", "returned overlap end",
                                     (positive)(ended - (p8 address_to)mine),
                                     (positive)destination + size);
                                same_bytes("memory_copy_end", "overlap and guards",
                                           mine, theirs, ROOM);
                        }

        // A zero-byte copy still writes its terminator and never needs a source.
        reference_fill(mine, 0xA5, ROOM);
        reference_fill(theirs, 0xA5, ROOM);
        same("memory_copy_end", "zero length end",
             (positive)(memory_copy_end((p8 address_to)mine + 31, null, 0) -
                        (p8 address_to)mine),
             31);
        theirs[31] = 0;
        same_bytes("memory_copy_end", "zero length guard", mine, theirs, ROOM);
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

                        memory_copy_apart(other, text, size + 1);
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
static positive caught_calls;

fn catch_writer(address_any data, positive length)
{
        b8 address_to from = (b8 address_to)data;

        caught_calls++;

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
        caught_calls = 0;
        caught[0] = 0;
}

fn check_shell_set_cursor()
{
        catch_reset();
        shell_set_cursor(catch_writer, 0, 0);
        same("shell_set_cursor", "zero coordinates",
             (positive)string_compare(caught, (string_address)"\033[0;0H"), 0);
        same("shell_set_cursor", "one atomic writer call", caught_calls, 1);

        catch_reset();
        shell_set_cursor(catch_writer, 12, 34);
        same("shell_set_cursor", "coordinate order",
             (positive)string_compare(caught, (string_address)"\033[34;12H"), 0);
        same("shell_set_cursor", "ordinary one-call write", caught_calls, 1);

        catch_reset();
        shell_set_cursor(catch_writer, 18446744073709551615ull,
                         18446744073709551615ull);
        same("shell_set_cursor", "full-width coordinates",
             (positive)string_compare(
                     caught,
                     (string_address)"\033[18446744073709551615;"
                                     "18446744073709551615H"),
             0);
        same("shell_set_cursor", "full-width one-call write", caught_calls, 1);
}

/*
        The last of a byte, and nothing for the terminator.

        This used to answer the terminator's own address when asked for byte
        zero, which is what strrchr does and what this is not. The library
        says so where it defines the two: string_last_of answers nothing,
        string_last_of_or_end answers the terminator, and the libc strrchr is
        an alias onto the second because that is the one POSIX describes.
*/
string_address reference_string_last_of(string_address source, p8 character)
{
        string_address found = null;

        if (!character)
                return null;

        while (string_get(source))
        {
                if string_is (source, character)
                        found = source;

                source++;
        }

        return found;
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

fn check_find_overlaps();
fn check_search();
fn check_append();

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

                /*
                        Back to one past where this candidate began, not to
                        where it stopped.

                        This used to carry on from the mismatch, and the
                        assembly was written to match it -- "carry on from
                        here, as the C did" is still in the commit that
                        replaced it. Both were wrong the same way, so the
                        differential test below could not see it: "aab" is
                        not found in "aaab" if the candidate at zero eats two
                        bytes and the scan resumes past the one at one.
                */
                step = find + 1;
                step_input = input;
        }

        return null;
}

/*
        The edges the assembly reaches for and the block above does not.

        check_strings works on one static array, so every string in it starts
        at the same alignment and none of the three word-at-a-time routines
        ever has to mask off the bytes before the string, land its terminator
        on the first byte of a word, or match a character living in the same
        word as the terminator. Those are exactly the places a word loop is
        wrong, so they are walked here on purpose: every start offset zero
        through seven, every length either side of the word boundary, and the
        two pointers of a compare at different offsets from each other.
*/
static p8 room[256];
static p8 twin[256];

fn check_string_edges()
{
        for (positive offset = 0; offset < 8; offset++)
                for (positive size = 0; size <= 40; size++)
                {
                        // 64 is a multiple of eight, so the offset is the whole
                        // of the distance into the word.
                        string_address text = room + 64 + offset;
                        string_address other = twin + 64 + ((offset + 3) & 7);

                        reference_fill(room, 0xA5, sizeof(room));
                        reference_fill(twin, 0xA5, sizeof(twin));

                        for (positive i = 0; i < size; i++)
                                text[i] = (p8)(next() % 4 + 'a');

                        text[size] = 0;

                        // A length of zero puts the terminator at the offset,
                        // and a size that makes offset + size a multiple of
                        // eight puts it on the first byte of the next word --
                        // the two cases the mask has to get right.
                        same("string_length", "offset", string_length(text),
                             reference_length(text));

                        for (positive i = 0; i < size; i++)
                                same("string_first_of", "offset present",
                                     (positive)string_first_of(text, text[i]),
                                     (positive)reference_first_of(text, text[i]));

                        // Absent, and absent as 0xA5 in particular: the bytes
                        // before the string and after the terminator are that
                        // value, so a routine that masks its input rather than
                        // its result answers with one of them.
                        same("string_first_of", "offset absent",
                             (positive)string_first_of(text, 'q'),
                             (positive)reference_first_of(text, 'q'));

                        same("string_first_of", "offset poison",
                             (positive)string_first_of(text, 0xA5),
                             (positive)reference_first_of(text, 0xA5));

                        same("string_first_of", "offset terminator",
                             (positive)string_first_of(text, 0),
                             (positive)reference_first_of(text, 0));

                        // The character in the same word as the terminator,
                        // which is where a scan that stops at the terminator
                        // first gets the order wrong.
                        if (size)
                        {
                                p8 keep = text[size - 1];

                                text[size - 1] = '#';
                                same("string_first_of", "beside the terminator",
                                     (positive)string_first_of(text, '#'),
                                     (positive)reference_first_of(text, '#'));
                                text[size - 1] = keep;
                        }

                        memory_copy_apart(other, text, size + 1);

                        same("string_compare", "offset equal",
                             (positive)string_compare(text, other),
                             (positive)reference_compare(text, other));

                        for (positive at = 0; at < size; at++)
                        {
                                p8 keep = other[at];

                                other[at] = (p8)(keep ^ 1);
                                same("string_compare", "offset greater",
                                     (positive)(string_compare(text, other) > 0),
                                     (positive)(reference_compare(text, other) > 0));
                                same("string_compare", "offset less",
                                     (positive)(string_compare(text, other) < 0),
                                     (positive)(reference_compare(text, other) < 0));

                                // The same place, but ending the second string
                                // there rather than changing it: one string a
                                // prefix of the other is the case a word
                                // compare answers by looking for the
                                // terminator inside an equal word.
                                other[at] = 0;
                                same("string_compare", "offset prefix",
                                     (positive)(string_compare(text, other) > 0),
                                     (positive)(reference_compare(text, other) > 0));
                                same("string_compare", "offset prefix reversed",
                                     (positive)(string_compare(other, text) < 0),
                                     (positive)(reference_compare(other, text) < 0));

                                other[at] = keep;
                        }
                }
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

        check_find_overlaps();
        check_search();
        check_append();
}

/*
        A candidate that fails, with the real match inside the part it ate.

        Everything above hunts for a needle that either is not there or
        begins where the scan first looks. Neither asks the question this
        gets wrong: after matching some of the needle and then failing, where
        does the next attempt start? One byte past where the candidate began,
        or where it gave up? The two differ only when a later start lies
        inside the run that matched, so the strings here are built to put one
        there.

        Left as literals rather than generated. Each one is a shape that
        broke something -- a repeated prefix, a needle that is its own tail,
        a run one byte longer than the needle -- and a generator that made
        them by accident would not say which was which.
*/
fn check_find_overlaps()
{
        static struct
        {
                string_address haystack;
                string_address needle;
        } cases[] = {
            {(string_address) "aaab", (string_address) "aab"},
            {(string_address) "aaaab", (string_address) "aab"},
            {(string_address) "aaaaaaaaab", (string_address) "aab"},
            {(string_address) "abababc", (string_address) "ababc"},
            {(string_address) "aabaabaaab", (string_address) "aabaaab"},
            {(string_address) "xxxxxy", (string_address) "xxy"},
            {(string_address) "banana", (string_address) "nana"},
            {(string_address) "aaa", (string_address) "aa"},
            {(string_address) "mississippi", (string_address) "issip"},
            {(string_address) "aabaacaad", (string_address) "aad"},
            // and the same shapes at every offset into a word, because the
            // scan aligns down and the first eight bytes are a special case
            {(string_address) " aaab", (string_address) "aab"},
            {(string_address) "  aaab", (string_address) "aab"},
            {(string_address) "   aaab", (string_address) "aab"},
            {(string_address) "    aaab", (string_address) "aab"},
            {(string_address) "     aaab", (string_address) "aab"},
            {(string_address) "      aaab", (string_address) "aab"},
            {(string_address) "       aaab", (string_address) "aab"},
        };

        for (positive i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
                string_address hay = cases[i].haystack;
                string_address needle = cases[i].needle;

                same("string_find", "a match inside a failed candidate",
                     (positive)string_find(hay, needle),
                     (positive)reference_find(hay, needle));
        }
}

/*
        string_search, which is strstr, and string_find, which is not.

        The two differ on an empty needle and only there: strstr answers the
        front of the haystack and string_find answers nothing. That is one
        byte of behaviour, which is exactly the size of thing that ships
        working on the machine it was written on and broken on the other two.
        The arm64 and riscv bodies had never been run when this was added.

        The long cases walk the needle through a haystack that is otherwise
        one repeated byte, at every offset into a word, because the front of
        the haystack is answered by a byte loop and everything past it by a
        body that aligns down and hunts the rarest byte.
*/
string_address reference_search(string_address hay, string_address needle)
{
        if (!string_get(needle))
                return hay;

        return reference_find(hay, needle);
}

fn check_search()
{
        static struct
        {
                string_address haystack;
                string_address needle;
        } cases[] = {
            {(string_address) "abc", (string_address) ""},
            {(string_address) "", (string_address) ""},
            {(string_address) "", (string_address) "a"},
            {(string_address) "abc", (string_address) "a"},
            {(string_address) "abc", (string_address) "c"},
            {(string_address) "abc", (string_address) "z"},
            {(string_address) "abc", (string_address) "abc"},
            {(string_address) "abc", (string_address) "abcd"},
            {(string_address) "abcdef", (string_address) "abc"},
            {(string_address) "abcdef", (string_address) "def"},
            {(string_address) "abcdef", (string_address) "cde"},
            {(string_address) "aaab", (string_address) "aab"},
            {(string_address) "aaaa", (string_address) "aab"},
            {(string_address) "banana", (string_address) "nana"},
            {(string_address) "mississippi", (string_address) "issip"},
        };

        for (positive i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
                same("string_search", "against a byte at a time strstr",
                     (positive)string_search(cases[i].haystack, cases[i].needle),
                     (positive)reference_search(cases[i].haystack, cases[i].needle));

        static p8 room[600];

        for (positive pad = 0; pad < 9; pad++)
                for (positive at = 16; at < 560; at++)
                {
                        for (positive i = 0; i < sizeof(room); i++) room[i] = 'a';

                        room[sizeof(room) - 1] = 0;
                        room[at] = 'q';
                        room[at + 1] = 'r';
                        room[at + 2] = 's';

                        string_address hay = (string_address)(room + pad);

                        same("string_search", "a needle far into a long haystack",
                             (positive)string_search(hay, (string_address) "qrs"),
                             (positive)reference_search(hay, (string_address) "qrs"));
                }
}

/*
        string_append, which is strcat: the source onto the end of the
        destination, and the destination handed back.

        Checked at every length either side can be, and with the join landing
        at every offset into a word, because it is two routines joined at a
        pointer one of them worked out. Guard bytes after the room, because
        strcat's whole character is that nothing bounds it and the way to get
        it wrong is to write one byte too many.
*/
fn check_append()
{
        static p8 room[160];
        static p8 want[160];

        for (positive first = 0; first < 40; first++)
                for (positive second = 0; second < 40; second++)
                {
                        for (positive i = 0; i < sizeof(room); i++)
                                room[i] = want[i] = 0xc7;

                        static p8 tail[48];

                        for (positive i = 0; i < first; i++)
                                room[i] = want[i] = (p8)('a' + i % 23);

                        room[first] = want[first] = 0;

                        for (positive i = 0; i < second; i++)
                                tail[i] = (p8)('A' + i % 19);

                        tail[second] = 0;

                        //      what it should look like afterwards
                        for (positive i = 0; i <= second; i++)
                                want[first + i] = tail[i];

                        string_address back =
                            string_append((string_address)room, (string_address)tail);

                        same("string_append", "hands the destination back",
                             (positive)back, (positive)room);

                        for (positive i = 0; i < sizeof(room); i++)
                                same("string_append", "byte for byte, guards included",
                                     (positive)room[i], (positive)want[i]);
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
/*
        Where this may write, which is not always /tmp.

        The file cases used to name /tmp directly. On a machine where /tmp is
        a small tmpfs, or one with a quota on it, or one where something else
        has filled it, those writes fail and the cases report as ordinary
        disagreements -- seven of them, with nothing anywhere saying the disk
        was the reason. That is the worst shape a test failure can take: it
        looks exactly like the thing it is supposed to be looking for.

        TMPDIR if it is set, /tmp if it is not, composed once here so the
        harness around this can point it somewhere with room.
*/
static p8 scratch_root[512];

static string_address scratch_path(string_address name)
{
        static p8 built[768];
        string_address address_to environment = program_environment_list();
        string_address root = environment
                                  ? string_get_environment(environment,
                                                           (string_address) "TMPDIR")
                                  : null;

        if (!root || !root[0])
                root = (string_address) "/tmp";

        positive at = 0;

        while (root[at] && at < sizeof(built) - 64)
        {
                built[at] = root[at];
                at++;
        }

        while (at && built[at - 1] == '/')
                at--;

        built[at++] = '/';

        for (positive i = 0; name[i] && at < sizeof(built) - 1; i++)
                built[at++] = name[i];

        built[at] = 0;
        (void)scratch_root;
        return (string_address)built;
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
                      (positive)scratch_path((string_address)"moonwater_verify_load"), 0);

        b32 made = system_call_4(syscall(openat), AT_FDCWD,
                                 (positive)scratch_path((string_address)"moonwater_verify_load"),
                                 FILE_CREATE | FILE_WRITE | FILE_TRUNCATE, 0644);

        checks++;

        if (made < 0)
        {
                report("file_load", "could not make a file", (positive)made, 0);
                return;
        }

        system_call_3(syscall(write), made, (positive)body, sizeof(body));
        system_call_1(syscall(close), made);

        file_new(address_of subject, scratch_path((string_address)"moonwater_verify_load"),
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
                      (positive)scratch_path((string_address)"moonwater_verify_load"), 0);
}

/*
        The routines merged from the conversion lanes.

        Each was verified by whoever wrote it, in a tree of their own. These
        are the same routines checked here, in one harness, so the claim does
        not rest on seven separate reports.
*/
#define SCRATCH scratch_path((string_address)"moonwater_verify_scratch")

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

fn check_lazy_file_and_library()
{
        static file subject;
        string_address lazy_path =
                scratch_path((string_address)"moonwater_verify_lazy");

        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)lazy_path, 0);
        b32 made = system_call_4(syscall(openat), AT_FDCWD,
                                 (positive)lazy_path,
                                 FILE_CREATE | FILE_WRITE | FILE_TRUNCATE,
                                 0644);
        checks++;
        if (made < 0)
        {
                report("file_new_lazy", "could not make fixture",
                       (positive)made, 0);
                return;
        }
        system_call_1(syscall(close), made);

        subject.handle = (positive)-1;
        subject.path = null;
        subject.flags = 0;
        subject.data = null;
        subject.loaded = false;
        subject.status.size = 0x1122334455667788ull;
        file_new_lazy(address_of subject, lazy_path, FILE_READ);
        same("file_new_lazy", "opens existing file",
             (positive)((bipolar)subject.handle >= 0), 1);
        same("file_new_lazy", "keeps path", (positive)subject.path,
             (positive)lazy_path);
        same("file_new_lazy", "keeps flags", subject.flags, FILE_READ);
        same("file_new_lazy", "does not stat",
             (positive)subject.status.size, 0x1122334455667788ull);
        file_close(address_of subject);
        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)lazy_path, 0);

        string_address missing =
                scratch_path((string_address)"moonwater_verify_lazy_missing");
        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)missing, 0);
        subject.handle = (positive)-1;
        file_new_lazy(address_of subject, missing, FILE_READ);
        same("file_new_lazy", "preserves open errno",
             (positive)((bipolar)subject.handle < 0), 1);
        same("file_valid", "rejects every negative errno",
             (positive)file_valid(address_of subject), 0);
        same("file_new_lazy", "failed path", (positive)subject.path,
             (positive)missing);
        subject.handle = (positive)-1;

        // Non-Windows library loading is deliberately only a thin file-open
        // placeholder today.  Pin both sides of that small contract: a slash
        // leaves storage untouched, while a bare name opens and stats it.
        static string_address bare =
                (string_address)"moonwater_verify_library";
        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)bare, 0);
        made = system_call_4(syscall(openat), AT_FDCWD, (positive)bare,
                             FILE_CREATE | FILE_WRITE | FILE_TRUNCATE, 0644);
        checks++;
        if (made < 0)
        {
                report("library_open", "could not make fixture",
                       (positive)made, 0);
                return;
        }
        system_call_1(syscall(close), made);

        subject.handle = (positive)-1;
        subject.path = null;
        library_open(address_of subject, (string_address)"./moonwater_verify_library");
        same("library_open", "slash path is left alone", subject.handle,
             (positive)-1);
        same("library_open", "slash path does not store a path",
             (positive)subject.path, 0);

        library_open(address_of subject, bare);
        same("library_open", "opens a bare name",
             (positive)((bipolar)subject.handle >= 0), 1);
        same("library_open", "stores bare name", (positive)subject.path,
             (positive)bare);
        same("library_open", "uses executable-open flags", subject.flags,
             FILE_READ | FILE_EXECUTE);
        same("library_get", "unsupported lookup is null",
             (positive)library_get(address_of subject,
                                   (string_address)"symbol"), 0);
        library_close(address_of subject);
        file_close(address_of subject);
        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)bare, 0);
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

/*
        The set scan, against the loop it replaces.

        Every set is exercised, not just a convenient one: the empty set stops
        immediately, the full set runs to the end, and the interesting ones are
        the metacharacter sets a shell lexer actually uses.
*/
positive reference_span(string_address source, const b8 address_to set)
{
        positive n = 0;

        while (1)
        {
                p8 c = source[n];

                if (!set[c])
                        return n;

                n++;
        }
}

static string_address verify_byte_class_names[BYTE_CLASSES] = {
    "alpha", "digit", "alnum", "upper", "lower", "space",
    "blank", "print", "graph", "cntrl", "punct", "xdigit",
};

// The three C bodies replaced by the byte-helper assembly, kept here as the
// independent specification the architecture entries are checked against.
static b32 reference_byte_class_index(string_address name, positive length)
{
        for (b32 which = 0; which < BYTE_CLASSES; which++)
        {
                string_address want = verify_byte_class_names[which];
                positive at = 0;

                while (at < length && want[at] && name[at] == want[at])
                        at++;

                if (at == length && !want[at])
                        return which;
        }

        return -1;
}

static bool reference_byte_class_holds(b32 which, p8 value)
{
        bool upper = value >= 'A' && value <= 'Z';
        bool lower = value >= 'a' && value <= 'z';
        bool digit = value >= '0' && value <= '9';
        bool printing = value >= ' ' && value < 127;

        switch (which)
        {
        case BYTE_ALPHA:
                return upper || lower;
        case BYTE_DIGIT:
                return digit;
        case BYTE_ALNUM:
                return upper || lower || digit;
        case BYTE_UPPER:
                return upper;
        case BYTE_LOWER:
                return lower;
        case BYTE_SPACE:
                return value == ' ' || (value >= '\t' && value <= '\r');
        case BYTE_BLANK:
                return value == ' ' || value == '\t';
        case BYTE_PRINT:
                return printing;
        case BYTE_GRAPH:
                return printing && value != ' ';
        case BYTE_CNTRL:
                return value < ' ' || value == 127;
        case BYTE_PUNCT:
                return printing && value != ' ' && !upper && !lower && !digit;
        case BYTE_XDIGIT:
                return digit || (value >= 'a' && value <= 'f') ||
                       (value >= 'A' && value <= 'F');
        }

        return false;
}

fn check_byte_helpers()
{
        static p8 name_room[64];
        static p8 members[256];
        static b8 set[STRING_SET_BYTES];
        static b8 untouched[STRING_SET_BYTES];
        static string_address unknown[] = {
            "",       "alph",  "alphax", "alpha!", "Digit", "xdigi",
            "xdigitx", "thing", "prints", "printable", null,
        };

        // Every known spelling, both as a normal string and as an exact slice
        // with hostile bytes immediately after it. Shifting the latter reaches
        // every alignment without ever giving the routine a terminator.
        for (b32 which = 0; which < BYTE_CLASSES; which++)
        {
                string_address name = verify_byte_class_names[which];
                positive length = reference_length(name);

                same("byte_class_index", "known name",
                     (positive)(bipolar)byte_class_index(name, length),
                     (positive)(bipolar)which);

                for (positive shift = 0; shift < 8; shift++)
                {
                        p8 address_to slice = name_room + 8 + shift;

                        reference_fill(name_room, 0xa5, sizeof(name_room));
                        for (positive at = 0; at < length; at++)
                                slice[at] = (p8)name[at];

                        same("byte_class_index", "bounded nonterminated name",
                             (positive)(bipolar)byte_class_index(
                                 (string_address)slice, length),
                             (positive)(bipolar)which);
                }

                same("byte_class_index", "known prefix is not the class",
                     (positive)(bipolar)byte_class_index(name, length - 1),
                     (positive)(bipolar)-1);

                reference_fill(name_room, '!', sizeof(name_room));
                for (positive at = 0; at < length; at++)
                        name_room[at] = (p8)name[at];

                same("byte_class_index", "known extension is not the class",
                     (positive)(bipolar)byte_class_index(
                         (string_address)name_room, length + 1),
                     (positive)(bipolar)-1);

                name_room[length - 1] ^= 1;
                same("byte_class_index", "one wrong byte",
                     (positive)(bipolar)byte_class_index(
                         (string_address)name_room, length),
                     (positive)(bipolar)-1);

                // All twelve classes over the complete byte domain, including
                // the high half where signed comparisons tend to go wrong.
                for (positive value = 0; value < 256; value++)
                        same("byte_class_holds", "all classes and bytes",
                             byte_class_holds(which, (p8)value),
                             reference_byte_class_holds(which, (p8)value));
        }

        for (positive at = 0; unknown[at]; at++)
        {
                positive length = reference_length(unknown[at]);

                same("byte_class_index", "unknown name",
                     (positive)(bipolar)byte_class_index(unknown[at], length),
                     (positive)(bipolar)reference_byte_class_index(
                         unknown[at], length));
        }

        same("byte_class_index", "empty slice needs no address",
             (positive)(bipolar)byte_class_index(null, 0),
             (positive)(bipolar)-1);

        for (positive value = 0; value < 256; value++)
        {
                same("byte_class_holds", "negative class",
                     byte_class_holds(-1, (p8)value), 0);
                same("byte_class_holds", "class past the table",
                     byte_class_holds(BYTE_CLASSES, (p8)value), 0);
        }

        // A terminated string can carry every nonzero byte exactly once. Zero
        // ends the member list and must remain absent from the set.
        for (positive value = 1; value < 256; value++)
                members[value - 1] = (p8)value;
        members[255] = 0;
        reference_fill(set, 0, sizeof(set));
        string_set_add(set, (string_address)members);

        for (positive value = 0; value < 256; value++)
                same("string_set_add", "every nonzero byte",
                     (positive)set[value], value != 0);

        reference_fill(set, 0x5a, sizeof(set));
        reference_copy(untouched, set, sizeof(set));
        string_set_add(set, (string_address)"");
        same_bytes("string_set_add", "empty member list changes nothing",
                   set, untouched, sizeof(set));
}

fn check_span()
{
        static p8 subject[600];
        static b8 set[STRING_SET_BYTES];
        static string_address sets[] = {
            "",                          // nothing is a member
            " \t",                       // blanks, as a lexer skips them
            "|&;<>()$`\\\\\"' \t\n",        // shell metacharacters
            "abcdefghijklmnopqrstuvwxyz",
            "0123456789",
        };

        for (positive which = 0; which < sizeof(sets) / sizeof(sets[0]); which++)
        {
                reference_fill(set, 0, sizeof(set));
                string_set_add(set, sets[which]);

                for (positive e = 0; e < EDGE_COUNT; e++)
                {
                        positive size = edges[e];

                        if (size >= sizeof(subject) - 1)
                                continue;

                        for (positive i = 0; i < size; i++)
                                subject[i] = (p8)(next() % 94 + 33);

                        subject[size] = 0;

                        same("string_span", "against the loop",
                             string_span(subject, set), reference_span(subject, set));

                        // And a string entirely of members, so the run reaches
                        // the terminator rather than stopping early by luck.
                        if (string_get(sets[which]))
                        {
                                positive length = string_length(sets[which]);

                                for (positive i = 0; i < size; i++)
                                        subject[i] = sets[which][i % length];

                                subject[size] = 0;

                                same("string_span", "all members",
                                     string_span(subject, set), size);
                        }
                }
        }
}

/*
        The word scanner, against the C that describes it.

        The same classes, the same quoting rules, one written for a person and
        one for the machine. Both answer with what they read and what they
        wrote, and both have to agree about every byte in between.
*/
/*
        Two spellings, for the same reason as the block at the end of this
        file: library.c calls this moonwater_lex_word and library.next.c calls
        it string_lex_word, and the suite has to build against both for as
        long as both exist.
*/
typedef positive (address_to lex_routine)(string_address, p8 address_to,
                                          const b8 address_to);

WEAK positive verify_lex_public(string_address source, p8 address_to into,
                                const b8 address_to class)
    __asm__("string_lex_word");
WEAK positive verify_lex_old(string_address source, p8 address_to into,
                             const b8 address_to class)
    __asm__("moonwater_lex_word");

static lex_routine lex_word_here()
{
        return verify_lex_public ? verify_lex_public : verify_lex_old;
}

static positive reference_lex_word(string_address source, p8 address_to into,
                                   const b8 address_to class)
{
        positive read = 0, wrote = 0;

        while (1)
        {
                p8 c = source[read];
                b8 kind = class[c];

                if (kind == 0)
                {
                        into[wrote++] = c;
                        read++;
                        continue;
                }

                if (kind == 1 || kind == 2 || kind == 5)
                        break;

                if (kind == 4)
                {
                        into[wrote++] = c;
                        read++;

                        if (source[read])
                        {
                                into[wrote++] = source[read];
                                read++;
                        }

                        continue;
                }

                // A quote, and everything up to its partner.
                {
                        p8 mark = c;

                        into[wrote++] = c;
                        read++;

                        while (source[read] && source[read] != mark)
                        {
                                if (mark == 34 && source[read] == 92 &&
                                    source[read + 1])
                                {
                                        into[wrote++] = source[read];
                                        read++;
                                }

                                into[wrote++] = source[read];
                                read++;
                        }

                        if (source[read])
                        {
                                into[wrote++] = source[read];
                                read++;
                        }
                }
        }

        into[wrote] = 0;

        return (wrote << 32) | read;
}

fn check_lex_word()
{
        if (!lex_word_here())
        {
                absent("string_lex_word");
                return;
        }

        static b8 class[256];
        static p8 subject[512];
        static p8 got[600];
        static p8 want[600];
        // Built rather than written, so the escaping is C's problem once.
        static p8 shapes[24][24];
        static positive shape_count;

        {
                static const string_address plain[] = {
                    "plain", "two words", "x=1", "a|b", "a;b", "$var",
                    "", "   leading", "trailing   ", "`sub`", null};
                positive n = 0;

                for (; plain[n]; n++)
                        string_copy(shapes[n], plain[n]);

                // The awkward ones, a byte at a time.
                string_copy(shapes[n], "a"); shapes[n][1] = 39; shapes[n][2] = 'b';
                shapes[n][3] = 39; shapes[n][4] = 'c'; shapes[n][5] = 0; n++;

                string_copy(shapes[n], "a"); shapes[n][1] = 34; shapes[n][2] = 'b';
                shapes[n][3] = 34; shapes[n][4] = 'c'; shapes[n][5] = 0; n++;

                shapes[n][0] = 39; shapes[n][1] = 'x'; shapes[n][2] = 0; n++;
                shapes[n][0] = 34; shapes[n][1] = 'x'; shapes[n][2] = 0; n++;
                shapes[n][0] = 39; shapes[n][1] = 39; shapes[n][2] = 0; n++;
                shapes[n][0] = 34; shapes[n][1] = 34; shapes[n][2] = 0; n++;
                shapes[n][0] = 'a'; shapes[n][1] = 92; shapes[n][2] = ' ';
                shapes[n][3] = 'b'; shapes[n][4] = 0; n++;
                shapes[n][0] = 'a'; shapes[n][1] = 92; shapes[n][2] = 0; n++;
                shapes[n][0] = 34; shapes[n][1] = 'a'; shapes[n][2] = 92;
                shapes[n][3] = 34; shapes[n][4] = 'b'; shapes[n][5] = 34;
                shapes[n][6] = 0; n++;
                shapes[n][0] = 39; shapes[n][1] = 34; shapes[n][2] = 39;
                shapes[n][3] = 0; n++;

                /*
                        Bytes above 127, through the copy path.

                        Not a test for signed loads, though it was meant to be:
                        every byte above 127 is an ordinary one and so is
                        whatever sits before the class table, so lb and lbu
                        answer the same and no input distinguishes them. Six
                        deliberate mutations of lbu to lb all passed. What
                        these do cover is that a high byte survives being
                        copied, which is worth having on its own.
                */
                shapes[n][0] = 'a'; shapes[n][1] = 0xE9; shapes[n][2] = 0xFF;
                shapes[n][3] = 'b'; shapes[n][4] = 0; n++;

                shapes[n][0] = 0x80; shapes[n][1] = 34; shapes[n][2] = 0xC3;
                shapes[n][3] = 34; shapes[n][4] = 0; n++;

                shape_count = n;
        }


        reference_fill(class, 0, sizeof(class));
        class[0] = 5;
        class[10] = 5;                  // newline
        class['#'] = 5;
        class[' '] = 1;
        class[9] = 1;                   // tab
        class[39] = 3;                  // single quote
        class[34] = 3;                  // double quote
        class[92] = 4;                  // backslash

        {
                static const string_address ops = "|&;<>()";

                for (positive i = 0; ops[i]; i++)
                        class[ops[i]] = 2;
        }

        for (positive i = 0; i < shape_count; i++)
        {
                positive mine, theirs;

                string_copy(subject, shapes[i]);
                reference_fill(got, 0xA5, sizeof(got));
                reference_fill(want, 0xA5, sizeof(want));

                mine = lex_word_here()(subject, got, class);
                theirs = reference_lex_word(subject, want, class);

                same("lex_word", "read", mine & 0xffffffff, theirs & 0xffffffff);
                same("lex_word", "wrote", mine >> 32, theirs >> 32);
                same_bytes("lex_word", "text", got, want, sizeof(got));
        }

        // And over generated input, so the shapes above are not the whole of it.
        for (positive round = 0; round < 400; round++)
        {
                static p8 alphabet[12];

                alphabet[0] = 'a'; alphabet[1] = 'b'; alphabet[2] = ' ';
                alphabet[3] = 39;  alphabet[4] = 34;  alphabet[5] = 92;
                alphabet[6] = '|'; alphabet[7] = ';'; alphabet[8] = '$';
                alphabet[9] = ' '; alphabet[10] = 'x'; alphabet[11] = 0;
                positive length = next() % 60;
                positive mine, theirs;

                for (positive i = 0; i < length; i++)
                        subject[i] = alphabet[next() % 11];

                subject[length] = 0;

                reference_fill(got, 0xA5, sizeof(got));
                reference_fill(want, 0xA5, sizeof(want));

                mine = lex_word_here()(subject, got, class);
                theirs = reference_lex_word(subject, want, class);

                same("lex_word", "generated read", mine & 0xffffffff,
                     theirs & 0xffffffff);
                same("lex_word", "generated wrote", mine >> 32, theirs >> 32);
                same_bytes("lex_word", "generated text", got, want, sizeof(got));
        }
}

/*
        strcmp where it decides, which is the first byte.

        The word loop now has a single byte ahead of it, so the answer for two
        strings that differ where they begin comes out of a different path than
        the answer for two that differ later. check_strings above compares only
        the sign; this compares the value, at every offset either path can end
        at, and over bytes above 127 -- where a signed byte load would give the
        wrong sign and the sign check would not see it.
*/
fn check_compare_edges()
{
        static p8 left[64];
        static p8 right[64];

        same("string_compare", "both empty",
             (positive)string_compare((string_address)"", (string_address)""),
             (positive)reference_compare((string_address)"", (string_address)""));

        same("string_compare", "empty against one",
             (positive)string_compare((string_address)"", (string_address)"a"),
             (positive)reference_compare((string_address)"", (string_address)"a"));

        same("string_compare", "one against empty",
             (positive)string_compare((string_address)"a", (string_address)""),
             (positive)reference_compare((string_address)"a", (string_address)""));

        // Every offset the byte path and the word path can part company at,
        // one past the eight byte step included.
        for (positive at = 0; at < 20; at++)
        {
                for (positive i = 0; i < 24; i++)
                {
                        left[i] = (p8)('a' + i % 26);
                        right[i] = left[i];
                }

                left[24] = 0;
                right[24] = 0;

                right[at] = (p8)(left[at] + 1);

                same("string_compare", "value at offset",
                     (positive)string_compare(left, right),
                     (positive)reference_compare(left, right));

                same("string_compare", "value at offset reversed",
                     (positive)string_compare(right, left),
                     (positive)reference_compare(right, left));

                // The same offset, but one string ends there instead.
                right[at] = left[at];
                right[at] = 0;

                same("string_compare", "ends at offset",
                     (positive)string_compare(left, right),
                     (positive)reference_compare(left, right));

                same("string_compare", "ends at offset reversed",
                     (positive)string_compare(right, left),
                     (positive)reference_compare(right, left));
        }

        // Bytes above 127 against bytes below, which is where a signed load
        // would answer with the wrong sign.
        for (positive a = 0; a < 256; a += 17)
        {
                for (positive b = 0; b < 256; b += 13)
                {
                        left[0] = (p8)a;
                        left[1] = 0;
                        right[0] = (p8)b;
                        right[1] = 0;

                        same("string_compare", "high byte",
                             (positive)string_compare(left, right),
                             (positive)reference_compare(left, right));

                        // And the same pair one byte in, so the word path
                        // reaches them too.
                        left[0] = 'z';
                        left[1] = (p8)a;
                        left[2] = 0;
                        right[0] = 'z';
                        right[1] = (p8)b;
                        right[2] = 0;

                        same("string_compare", "high byte past the first",
                             (positive)string_compare(left, right),
                             (positive)reference_compare(left, right));
                }
        }
}

/*
        The environment lookup against the loop it replaced.

        The reference is env_get's body out of src/sh/builtin.c, copied as it
        stood: a strchr for the equals, a length compare, then the bytes. That
        is the answer string_get_environment has to agree with, entry for entry
        and edge for edge -- an entry with no equals in it, an entry that is
        only an equals, a name that is a prefix of another name, and a name
        that is one byte longer than the entry it nearly matches.
*/
string_address reference_get_environment(string_address address_to list,
                                         string_address name)
{
        if (name == null)
                return null;

        positive name_len = reference_length(name);
        positive idx = 0;

        while (list[idx])
        {
                string_address entry = list[idx];
                string_address eq = reference_first_of(entry, '=');

                if (eq)
                {
                        positive key_len = eq - entry;

                        if (key_len == name_len)
                        {
                                positive i = 0;
                                for (i = 0; i < name_len; i++)
                                {
                                        if (string_get(entry + i) != string_get(name + i))
                                                break;
                                }

                                if (i == name_len)
                                        return eq + 1;
                        }
                }

                idx++;
        }

        return null;
}

fn check_environment_list()
{
        string_address address_to list = program_environment_list();
        positive count = 0;

        while (list && list[count])
        {
                same("program_environment_list", "same entry as indexed access",
                     (positive)list[count], (positive)program_environment((b32)count));
                count++;
        }

        same("program_environment_list", "same terminator as indexed access",
             (positive)(list ? list[count] : null),
             (positive)program_environment((b32)count));

        if (program_stack_base)
        {
                string_address address_to saved_words = program_words;
                b32 saved_count = program_words_count;
                static string_address borrowed[] = {(string_address) "verify"};

                program_arguments_use(borrowed, 0);

                same("program_environment_list", "borrowed arguments keep the environment",
                     (positive)program_environment_list(), (positive)list);
                same("program_environment_list", "borrowed vector agrees with index zero",
                     (positive)program_environment_list()[0],
                     (positive)program_environment(0));

                if (saved_words)
                        program_arguments_use(saved_words, saved_count);
                else
                        program_arguments_own();
        }

        {
                p8 address_to saved_stack = program_stack_base;

                program_stack_base = null;
                same("program_environment_list", "no process stack",
                     (positive)program_environment_list(), 0);
                program_stack_base = saved_stack;
        }
}

fn check_argument_list()
{
        string_address address_to saved_words = program_words;
        b32 saved_count = program_words_count;
        p8 address_to saved_stack = program_stack_base;
        static string_address borrowed[] = {
            (string_address) "storage", (string_address) "one", null};

        if (program_stack_base)
        {
                string_address address_to list = program_argument_list();

                same("program_argument_list", "process stack vector",
                     (positive)list, (positive)(program_stack_base + 8));
                same("program_argument_list", "process stack index zero",
                     (positive)list[0], (positive)program_argument(0));
        }

        program_arguments_use(borrowed, 2);
        same("program_argument_list", "borrowed vector",
             (positive)program_argument_list(), (positive)borrowed);
        same("program_argument_list", "borrowed index one",
             (positive)program_argument_list()[1],
             (positive)program_argument(1));

        program_arguments_own();
        program_stack_base = null;
        same("program_argument_list", "no vector",
             (positive)program_argument_list(), 0);

        program_stack_base = saved_stack;
        if (saved_words)
                program_arguments_use(saved_words, saved_count);
        else
                program_arguments_own();
}

fn check_environment()
{
        check_argument_list();
        check_environment_list();

        static p8 block[64][48];
        static string_address list[65];

        static const string_address shapes[] = {
            "PATH=/bin:/usr/bin:/",
            "SHELL=/bin/sh",
            "V=x",
            "VV=xx",
            "=leading",
            "NOEQUALS",
            "EMPTY=",
            "P=1",
            "PA=2",
            "PAT=3",
            "PATHX=4",
            "a=b",
            "LONGERNAMETHANANYWORD=value",
            "x",
            "",
            null,
        };

        static const string_address names[] = {
            "PATH",
            "SHELL",
            "V",
            "VV",
            "",
            "NOEQUALS",
            "EMPTY",
            "P",
            "PA",
            "PAT",
            "PATHX",
            "PATHXY",
            "a",
            "LONGERNAMETHANANYWORD",
            "LONGERNAMETHANANYWOR",
            "x",
            "MISSING",
            "PATH ",
            null,
        };

        positive count = 0;

        while (shapes[count])
        {
                string_copy(block[count], shapes[count]);
                list[count] = block[count];
                count++;
        }

        list[count] = null;

        // Every name against the whole list, then against every prefix of the
        // list, so the answer is checked where the entry is first, last and
        // absent.
        for (positive cut = 0; cut <= count; cut++)
        {
                string_address held = list[cut];

                list[cut] = null;

                for (positive n = 0; names[n]; n++)
                {
                        same("get_environment", "value",
                             (positive)string_get_environment(list, (string_address)names[n]),
                             (positive)reference_get_environment(list, (string_address)names[n]));
                }

                list[cut] = held;
        }

        // An empty list answers nothing, whatever it is asked.
        {
                static string_address nothing[1];

                nothing[0] = null;

                same("get_environment", "empty list",
                     (positive)string_get_environment(nothing, (string_address)"PATH"),
                     (positive)reference_get_environment(nothing, (string_address)"PATH"));
        }

        // Generated entries and names over a small alphabet, so the prefix and
        // equals cases turn up in combinations nobody chose.
        for (positive round = 0; round < 300; round++)
        {
                static p8 alphabet[5];

                alphabet[0] = 'a';
                alphabet[1] = 'b';
                alphabet[2] = '=';
                alphabet[3] = 'a';
                alphabet[4] = 'c';

                positive entries = next() % 8 + 1;

                for (positive e = 0; e < entries; e++)
                {
                        positive length = next() % 7;

                        for (positive i = 0; i < length; i++)
                                block[e][i] = alphabet[next() % 5];

                        block[e][length] = 0;
                        list[e] = block[e];
                }

                list[entries] = null;

                for (positive t = 0; t < 6; t++)
                {
                        static p8 wanted[8];

                        positive length = next() % 4;

                        for (positive i = 0; i < length; i++)
                                wanted[i] = alphabet[next() % 5];

                        wanted[length] = 0;

                        same("get_environment", "generated",
                             (positive)string_get_environment(list, wanted),
                             (positive)reference_get_environment(list, wanted));
                }
        }
}

/*
        The table lookup, against the loop it replaces.

        The entries are laid out as the shell lays its commands out: a name
        first, then something else, walked by a stride. The awkward cases are
        the ones a first-byte test could get wrong -- names that share a first
        byte, a name that is a prefix of another, and the empty name.
*/
typedef struct
{
        string_address name;
        positive marker;
} find_entry;

static positive reference_table_find(string_address name, address_any table,
                                     positive stride, positive count)
{
        for (positive i = 0; i < count; i++)
        {
                string_address entry =
                    address_to(string_address address_to)((p8 address_to)table + i * stride);

                if (!entry)
                        return count;

                if (!string_compare(entry, name))
                        return i;
        }

        return count;
}

/*
        Names put where reading eight bytes of them would leave the page.

        string_table_find reads a whole word of the wanted name and a whole
        word of each entry's, which is only safe while those eight bytes are
        in one page -- and both reads used to be unguarded. A static array is
        never in the wrong place by accident, so the two are put there on
        purpose: the last bytes of a page, worked out from the address the
        linker gave the buffer rather than assumed.

        The page after this one is mapped here, so what this catches is a
        wrong answer rather than a fault. That is enough: the byte paths the
        guard sends those cases down are only reached this way, and a wrong
        answer out of them says the guard is being taken.
*/
static p8 page_room[3 * 4096];

fn check_table_find_page_edge()
{
        static find_entry sitting[8];
        static string_address held[] = {"cat", "cd", "basename", "dirname",
                                        "e", "export", "z", null};
        positive base = (positive)(address_any)page_room;
        positive page = (4096 - (base & 4095)) & 4095;
        positive count = 0;

        if (page == 0)
                page = 4096;

        for (; held[count]; count++)
                ;

        //      Each entry's name at a shifting distance from the end of a
        //      page, so that some of them cross it and some do not.
        for (positive back = 1; back <= 9; back++)
        {
                p8 address_to at = page_room + page - back;

                for (positive i = 0; i < count; i++)
                {
                        positive length = reference_length(held[i]);

                        //      Only the name being looked for moves; the
                        //      others stay where they are, so the table is
                        //      the same table each time.
                        sitting[i].name = held[i];
                        sitting[i].marker = i;

                        if (i != 2)
                                continue;

                        for (positive k = 0; k <= length; k++)
                                at[k] = (p8)held[i][k];

                        sitting[i].name = (string_address)at;
                }

                for (positive i = 0; i < count; i++)
                {
                        same("table_find", "an entry at the end of a page",
                             string_table_find(held[i], sitting,
                                               sizeof(find_entry), count),
                             reference_table_find(held[i], sitting,
                                                  sizeof(find_entry), count));

                        //      And the wanted name itself at the end of a
                        //      page, which sends the whole call down the
                        //      other path.
                        {
                                positive length = reference_length(held[i]);
                                p8 address_to want = page_room + 2 * 4096 + page - back;

                                for (positive k = 0; k <= length; k++)
                                        want[k] = (p8)held[i][k];

                                same("table_find", "a name at the end of a page",
                                     string_table_find((string_address)want, sitting,
                                                       sizeof(find_entry), count),
                                     reference_table_find((string_address)want, sitting,
                                                          sizeof(find_entry), count));
                        }
                }
        }
}

fn check_table_find()
{
        static find_entry entries[16];
        static string_address names[] = {
            "cat", "cd", "cp", "chmod", "clear", "basename", "b", "",
            "echo", "exit", "export", "e", "zzz", "cata", "ca", null};
        positive count = 0;

        for (; names[count]; count++)
        {
                entries[count].name = names[count];
                entries[count].marker = count;
        }

        // A zero count must answer before touching the table, and the count is
        // an exact fence rather than merely a not-found return value.
        same("table_find", "zero count does not touch the table",
             string_table_find((string_address)"cat", null,
                               sizeof(find_entry), 0),
             0);
        same("table_find", "count fences the walk",
             string_table_find(names[5], entries, sizeof(find_entry), 5),
             5);

        // Every name in the table, which must be found at its own index.
        for (positive i = 0; i < count; i++)
                same("table_find", "present",
                     string_table_find(names[i], entries, sizeof(find_entry), count),
                     reference_table_find(names[i], entries, sizeof(find_entry), count));

        // And names that are not, including ones sharing a first byte with
        // entries that are, and prefixes and extensions of them.
        {
                static string_address absent[] = {
                    "catx", "c", "cha", "ba", "basenam", "basenames", "exporte",
                    "ec", "zz", "zzzz", "q", "", null};

                for (positive i = 0; absent[i]; i++)
                        same("table_find", "absent",
                             string_table_find(absent[i], entries,
                                               sizeof(find_entry), count),
                             reference_table_find(absent[i], entries,
                                                  sizeof(find_entry), count));
        }

        // A table that ends early on a null name rather than on the count.
        entries[5].name = null;
        same("table_find", "null entry stops the walk",
             string_table_find((string_address)"export", entries,
                               sizeof(find_entry), count),
             reference_table_find((string_address)"export", entries,
                                  sizeof(find_entry), count));

        entries[5].name = names[5];

        /*
                The same names, looked up through a copy sitting somewhere
                else.

                Every needle above is a literal, and literals are laid out end
                to end by the compiler -- so a routine that reads past one
                terminator reads the next literal, which is stable and often
                happens to give the right answer. A name of exactly seven
                characters did read past, and this is the shape that says so:
                the needle is copied into a buffer at a shifting offset, so
                what follows it is different every time.
        */
        {
                static p8 room[64];
                static find_entry sized[17];
                static p8 made[17][20];
                positive many = 0;

                // One name of every length from one to sixteen, so the eight
                // byte boundary is crossed from both sides and landed on.
                for (positive length = 1; length <= 16; length++)
                {
                        for (positive at = 0; at < length; at++)
                                made[many][at] = (p8)('a' + (length + at) % 26);

                        made[many][length] = 0;
                        sized[many].name = made[many];
                        sized[many].marker = many;
                        many++;
                }

                for (positive i = 0; i < many; i++)
                        for (positive shift = 0; shift + 20 < sizeof(room); shift++)
                        {
                                string_address moved = room + shift;

                                memory_fill(room, 'x', sizeof(room));
                                string_copy(moved, sized[i].name);

                                same("table_find", "needle copied elsewhere",
                                     string_table_find(moved, sized,
                                                       sizeof(find_entry), many),
                                     reference_table_find(moved, sized,
                                                          sizeof(find_entry), many));
                        }
        }
}

/*
        What is on either side of the string.

        Every needle above sits at the start of a fresh buffer, so nothing
        hostile has ever been in the bytes immediately in front of one. The
        word at a time hunts align their pointer down so the first load cannot
        cross a page, which means they do read those bytes, and then have to
        throw away what they found there. Whether that throwing away is exact
        is the whole question, and it can only be asked by putting something
        in them.

        The values are chosen and not random. A SWAR zero test borrows out of
        a zero byte into the byte above it, so a byte in the prefix equal to
        the one being hunted can raise a flag on the first byte of the string
        -- but only when that first byte is the next value up, which is one
        pair in two hundred and fifty six. Four hundred thousand random trials
        found none of it.

        The field is filled with the hunted byte end to end, so the bytes
        after the terminator are as hostile as the ones before it: a routine
        that reads a word past the end has to discard what it finds there too.
*/

/*
        The names, in both libraries.

        library.c writes an assembly override for a C function and says which
        with a MOONWATER_HAVE_ macro. library.next.c has no C function to
        override -- the assembly is the routine and the libc names are aliases
        onto it -- so it defines no such macro at all, and a case guarded on
        one compiles to nothing the moment next is in place. Ninety five
        thousand checks went missing that way and the lane still said
        everything agrees, which is the same shape as the bug this section
        was written for.

        So nothing here is guarded. Every routine is declared twice, once
        under each spelling, weak and bound to its symbol by name: a weak name
        the linker cannot find is null instead of an error, so one binary asks
        the library what it has rather than asking the preprocessor what it
        once had. Bound by name because a prototype would collide -- library.c
        declares a strrchr of its own and next declares none.

        A routine under neither name is named in the transcript and counted.
        The inventory says which machines are short of which, so that is not a
        failure; being short of one silently is.
*/

typedef string_address (address_to hunt_byte)(string_address, b32);
typedef string_address (address_to hunt_bounded)(string_address, positive, b32);
typedef address_any (address_to hunt_memory)(address_any, b32, positive);
typedef positive (address_to measure_bounded)(string_address, positive);
typedef b32 (address_to compare_bounded)(string_address, string_address, positive);

WEAK string_address verify_or_end_public(string_address source, b32 character)
    __asm__("string_first_of_or_end");
WEAK string_address verify_or_end_libc(string_address source, b32 character)
    __asm__("strchrnul");

WEAK string_address verify_first_max_public(string_address source, positive count,
                                            b32 character)
    __asm__("string_first_of_max");
WEAK string_address verify_first_max_libc(string_address source, positive count,
                                          b32 character)
    __asm__("strnchr");

WEAK string_address verify_last_or_end_public(string_address source, b32 character)
    __asm__("string_last_of_or_end");
WEAK string_address verify_last_or_end_libc(string_address source, b32 character)
    __asm__("strrchr");

WEAK address_any verify_memory_first_public(address_any source, b32 character,
                                            positive count)
    __asm__("memory_first_of");
WEAK address_any verify_memory_first_libc(address_any source, b32 character,
                                          positive count)
    __asm__("memchr");

WEAK positive verify_length_max_public(string_address source, positive bound)
    __asm__("string_length_max");
WEAK positive verify_length_max_libc(string_address source, positive bound)
    __asm__("strnlen");

WEAK b32 verify_compare_max_public(string_address a, string_address b, positive count)
    __asm__("string_compare_max");
WEAK b32 verify_compare_max_libc(string_address a, string_address b, positive count)
    __asm__("strncmp");

// Walks to the terminator and answers with it rather than with nothing, which
// is the whole difference between this and string_first_of.
string_address reference_or_end(string_address source, p8 character)
{
        while (string_get(source) && string_get(source) != character)
                source++;

        return source;
}

// The character is looked at before the terminator is, so a hunt for zero
// finds the terminator inside the bound. That is what the kernel's own does.
string_address reference_first_max(string_address source, positive count,
                                   p8 character)
{
        for (positive i = 0; i < count; i++)
        {
                if (source[i] == character)
                        return source + i;

                if (!source[i])
                        break;
        }

        return null;
}

address_any reference_memory_first(address_any source, p8 character, positive count)
{
        p8 address_to at = source;

        for (positive i = 0; i < count; i++)
                if (at[i] == character)
                        return at + i;

        return null;
}

positive reference_length_max(string_address source, positive bound)
{
        positive i = 0;

        while (i < bound && source[i])
                i++;

        return i;
}

b32 reference_compare_max(string_address a, string_address b, positive count)
{
        for (positive i = 0; i < count; i++)
        {
                if (a[i] != b[i])
                        return (b32)a[i] - (b32)b[i];

                if (!a[i])
                        return 0;
        }

        return 0;
}

string_address reference_last_or_end(string_address source, p8 character)
{
        string_address last = null;

        for (;; source++)
        {
                if (string_get(source) == character)
                        last = source;

                if (!string_get(source))
                        return last;
        }
}

static p8 field[512];
static p8 field_twin[512];

// Eight byte aligned, so an offset from here is the whole of the distance
// into a word however the linker placed the array.
static string_address aligned_at(p8 address_to array)
{
        positive at = (positive)(address_any)array;

        return array + ((8 - (at & 7)) & 7);
}

/*
        The bounded span and the digit run, and the two ways they are lied to.

        Both are new and both replaced a byte loop written out in six files, so
        the reference below is that loop and not a second idea about what it
        should do.

        Neither is ever handed a string literal. Literals sit end to end in
        .rodata, and a routine that reads one byte past its own terminator
        reads the front of the next one and usually gets the right answer
        anyway; a seven character name once survived forty two thousand checks
        that way. Everything here is built in a buffer at a shifting offset,
        with what sits in front of the subject and what sits behind the end of
        it chosen to be the worst possible answer rather than a zero.
*/
positive reference_span_max(string_address source, positive bound,
                            const b8 address_to set)
{
        positive n = 0;

        while (n < bound)
        {
                p8 c = source[n];

                if (!set[c])
                        return n;

                n++;
        }

        return n;
}

positive reference_digits(string_address source, positive bound,
                          positive address_to used)
{
        positive value = 0;
        positive n = 0;

        while (n < bound && source[n] >= '0' && source[n] <= '9')
        {
                value = value * 10 + (positive)(source[n] - '0');
                n++;
        }

        if (used)
                address_to used = n;

        return value;
}

static bipolar reference_bipolar(string_address source, positive address_to used)
{
        positive at = 0;
        positive digits = 0;
        positive value = 0;
        bool negative = false;

        if (source)
        {
                if (source[at] == '-' || source[at] == '+')
                {
                        negative = source[at] == '-';
                        at++;
                }

                while (source[at] >= '0' && source[at] <= '9')
                {
                        value = value * 10 + (positive)(source[at] - '0');
                        at++;
                        digits++;
                }
        }

        if (!digits)
                at = 0;

        if (used)
                address_to used = at;

        return negative && digits ? (bipolar)((positive)0 - value)
                                  : (bipolar)value;
}

static positive reference_base_digit(p8 character)
{
        if (character >= '0' && character <= '9')
                return (positive)(character - '0');

        if (character >= 'a' && character <= 'z')
                return (positive)(character - 'a' + 10);

        if (character >= 'A' && character <= 'Z')
                return (positive)(character - 'A' + 10);

        return (positive)-1;
}

positive reference_digits_base_max(string_address source, positive bound,
                                   positive base, positive address_to used)
{
        positive value = 0;
        positive n = 0;

        if (base >= 2 && base <= 36)
        {
                while (n < bound)
                {
                        positive digit = reference_base_digit(source[n]);

                        if (digit >= base)
                                break;

                        value = value * base + digit;
                        n++;
                }
        }

        if (used)
                address_to used = n;

        return value;
}

bool reference_digits_exact(string_address source, positive address_to value)
{
        positive used;

        if (!source)
                return false;

        positive parsed = reference_digits(source, (positive)-1, address_of used);

        if (!used || source[used])
                return false;

        if (value)
                address_to value = parsed;

        return true;
}

static p8 span_room[1024];

// Far enough in that a routine aligning its pointer down has plenty in front
// of the subject to be fooled by.
#define SPAN_HEAD 128

static p8 address_to span_subject(positive offset)
{
        return (p8 address_to)aligned_at(span_room) + SPAN_HEAD + offset;
}

static positive span_bounds[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 11,
                                 15, 16, 17, 23, 24, 25, 26, 31, 32, 33};

#define SPAN_BOUND_COUNT (sizeof(span_bounds) / sizeof(span_bounds[0]))

fn check_span_max()
{
        static b8 set[STRING_SET_BYTES];
        static string_address sets[] = {
            " \t",
            "0123456789",
            "abcdefghijklmnopqrstuvwxyz",
            "|&;<>()$`\\\"' \t\n",
        };

        positive count = sizeof(sets) / sizeof(sets[0]);

        // One pass past the end of that list, with every byte a member and the
        // terminator among them. The bound is then the only thing that can end
        // the run, and a routine that quietly stops at a zero the way
        // string_span does is caught here and nowhere else.
        for (positive which = 0; which <= count; which++)
        {
                positive members = 1;
                p8 breaker = 0;

                if (which == count)
                        reference_fill(set, 1, sizeof(set));
                else
                {
                        reference_fill(set, 0, sizeof(set));
                        string_set_add(set, sets[which]);
                        members = string_length(sets[which]);

                        for (positive v = 1; v < 256; v++)
                                if (!set[v])
                                {
                                        breaker = (p8)v;
                                        break;
                                }
                }

                // What sits outside the window. A member on one pass, so
                // anything reading past the bound or in front of the pointer
                // keeps counting; a non-member on the other, so anything
                // reading outside it stops short instead. Both directions,
                // because only one of the two shows up as a number too small.
                for (positive hostile = 0; hostile < 2; hostile++)
                {
                        p8 outside =
                            hostile ? (which == count ? (p8)0xff
                                                      : (p8)sets[which][0])
                                    : (p8)0x00;

                        for (positive offset = 0; offset < 8; offset++)
                                for (positive b = 0; b < SPAN_BOUND_COUNT; b++)
                                {
                                        positive bound = span_bounds[b];

                                        // The last stop is the run that
                                        // nothing inside the window ends.
                                        for (positive stop = 0; stop <= 34; stop++)
                                        {
                                                p8 address_to subject =
                                                    span_subject(offset);

                                                reference_fill(span_room, outside,
                                                               sizeof(span_room));

                                                for (positive i = 0; i < 40; i++)
                                                        subject[i] =
                                                            which == count
                                                                ? (p8)(i * 7 + 3)
                                                                : (p8)sets[which]
                                                                          [i % members];

                                                if (stop < 34 && which != count)
                                                        subject[stop] = breaker;

                                                same("string_span_max",
                                                     "against the loop",
                                                     string_span_max(subject, bound,
                                                                     set),
                                                     reference_span_max(subject, bound,
                                                                        set));
                                        }
                                }
                }
        }
}

/*
        The digit run.

        The buffer is digits from end to end and the run is a window inside it
        ended by one byte that is not a digit. So a routine that reads a word
        in front of its pointer, or one byte past where it was told to stop, is
        handed more digits and answers a number that is not the one asked for
        -- which is the only way this can be wrong and still look plausible.
*/
static p8 digit_breakers[] = {0x00, '/', ':', ' ', 'a', 0xff, '.', '-', '+'};

#define DIGIT_BREAKER_COUNT (sizeof(digit_breakers) / sizeof(digit_breakers[0]))

fn check_digits()
{
        // Null must be rejected before the first load, and failure must not
        // commit through the optional result pointer.
        {
                positive got = (positive)0xa5a5a5a5a5a5a5a5ull;
                positive want = got;

                same("string_digits_exact", "null source",
                     string_digits_exact(null, address_of got), false);
                same("string_digits_exact", "null leaves output alone", got, want);
                same("string_digits_exact", "null with no output",
                     string_digits_exact(null, null), false);
        }

        for (positive br = 0; br < DIGIT_BREAKER_COUNT; br++)
                for (positive offset = 0; offset < 8; offset++)
                        for (positive length = 0; length <= 26; length++)
                        {
                                p8 address_to subject = span_subject(offset);
                                positive want_used = 0;
                                positive got_used = 0;

                                reference_fill(span_room, '7', sizeof(span_room));

                                for (positive i = 0; i < length; i++)
                                        subject[i] = (p8)('0' + next() % 10);

                                subject[length] = digit_breakers[br];

                                same("string_digits", "value",
                                     string_digits(subject, address_of got_used),
                                     reference_digits(subject, (positive)-1,
                                                      address_of want_used));

                                same("string_digits", "bytes taken", got_used,
                                     want_used);

                                // The count is what tells a caller there were
                                // no digits at all: a single zero and no digits
                                // both answer nothing.
                                same("string_digits", "none is not a zero",
                                     (positive)(got_used == 0),
                                     (positive)(length == 0));

                                // Handed nowhere to put the count.
                                same("string_digits", "no count wanted",
                                     string_digits(subject, 0),
                                     reference_digits(subject, (positive)-1, 0));

                                // The exact parser accepts only the NUL-ended,
                                // nonempty run. Both answers begin poisoned so
                                // every failure also proves no output store.
                                {
                                        positive got_value =
                                            (positive)0xa5a5a5a5a5a5a5a5ull;
                                        positive want_value = got_value;

                                        same("string_digits_exact", "validity",
                                             string_digits_exact(
                                                 subject, address_of got_value),
                                             reference_digits_exact(
                                                 subject, address_of want_value));
                                        same("string_digits_exact",
                                             "value or untouched output",
                                             got_value, want_value);
                                        same("string_digits_exact",
                                             "optional output",
                                             string_digits_exact(subject, null),
                                             reference_digits_exact(subject, null));
                                }

                                for (positive bound = 0; bound <= length + 3; bound++)
                                {
                                        same("string_digits_max", "value",
                                             string_digits_max(subject, bound,
                                                               address_of got_used),
                                             reference_digits(subject, bound,
                                                              address_of want_used));

                                        same("string_digits_max", "bytes taken",
                                             got_used, want_used);
                                }

                                same("string_digits_max", "no count wanted",
                                     string_digits_max(subject, length, 0),
                                     reference_digits(subject, length, 0));
                        }
}

fn check_signed_digits_and_width()
{
        static string_address cases[] = {
            "", "+", "-", "0", "+0", "-0", "1", "+17", "-17",
            "1x", "+1x", "-1x", "18446744073709551615",
            "18446744073709551616", "-18446744073709551616",
            "999999999999999999999999999999", " 1", "\t-1",
        };

        positive got_used = (positive)-1;
        same("string_bipolar", "null value", string_bipolar(null, address_of got_used),
             0);
        same("string_bipolar", "null used", got_used, 0);
        same("string_bipolar", "null optional used", string_bipolar(null, null), 0);

        for (positive which = 0; which < sizeof(cases) / sizeof(cases[0]); which++)
                for (positive offset = 0; offset < 8; offset++)
                {
                        p8 address_to subject = span_subject(offset);
                        positive length = string_length(cases[which]);
                        positive want_used = 0;

                        reference_fill(span_room, 0xa5, sizeof(span_room));
                        memory_copy(subject, cases[which], length + 1);

                        got_used = (positive)-1;
                        bipolar want = reference_bipolar(subject,
                                                         address_of want_used);
                        same("string_bipolar", "value",
                             string_bipolar(subject, address_of got_used), want);
                        same("string_bipolar", "used", got_used, want_used);
                        same("string_bipolar", "optional used",
                             string_bipolar(subject, null), want);
                }

        positive power = 1;
        for (positive digits = 1; digits <= 20; digits++)
        {
                same("positive_digits", "power", positive_digits(power), digits);
                if (power > 1)
                        same("positive_digits", "before power",
                             positive_digits(power - 1), digits - 1);
                if (digits < 20)
                {
                        same("positive_digits", "after power",
                             positive_digits(power + 1), digits);
                        power *= 10;
                }
        }
        same("positive_digits", "maximum", positive_digits((positive)-1), 20);
}

/*
        A bounded digit run in every supported base.

        The byte at the last place is walked through all 256 possibilities for
        every base, so punctuation near the alphabet -- especially '@', '`',
        '[' and '{' -- cannot accidentally become a digit through ASCII case
        folding. The bytes after the bound are valid zeroes: crossing the
        fence therefore changes both the consumed count and usually the value.

        Every source alignment through sixteen bytes is crossed with those
        bytes and bases. A separate long run crosses every bound through 96;
        that reaches unsigned wrap even in base two and proves wrapping and
        the strict pre-load fence together.
*/
static p8 base_digit_character(positive digit, bool upper)
{
        if (digit < 10)
                return (p8)('0' + digit);

        return (p8)((upper ? 'A' : 'a') + digit - 10);
}

fn check_digits_base()
{
        static positive invalid_bases[] = {0, 1, 37, 255, (positive)-1};

        // No source exists to load. A valid base with a zero bound and an
        // invalid base with any bound must both finish before the first load.
        for (positive base = 2; base <= 36; base++)
        {
                positive got_used = (positive)-1;

                same("string_digits_base_max", "zero bound null value",
                     string_digits_base_max(null, 0, base, address_of got_used), 0);
                same("string_digits_base_max", "zero bound null used", got_used, 0);
                same("string_digits_base_max", "zero bound no used",
                     string_digits_base_max(null, 0, base, null), 0);

                if (base == 8)
                {
                        got_used = (positive)-1;
                        same("string_digits_octal_max", "zero bound null value",
                             string_digits_octal_max(null, 0, address_of got_used), 0);
                        same("string_digits_octal_max", "zero bound null used",
                             got_used, 0);
                        same("string_digits_octal_max", "zero bound no used",
                             string_digits_octal_max(null, 0, null), 0);
                        got_used = (positive)-1;
                        same("string_digits_octal_escape_max",
                             "zero bound null value",
                             string_digits_octal_escape_max(
                                 null, 0, address_of got_used), 0);
                        same("string_digits_octal_escape_max",
                             "zero bound null used", got_used, 0);
                        same("string_digits_octal_escape_max",
                             "zero bound no used",
                             string_digits_octal_escape_max(null, 0, null), 0);
                }
                else if (base == 16)
                {
                        got_used = (positive)-1;
                        same("string_digits_hexadecimal_max", "zero bound null value",
                             string_digits_hexadecimal_max(null, 0,
                                                           address_of got_used),
                             0);
                        same("string_digits_hexadecimal_max", "zero bound null used",
                             got_used, 0);
                        same("string_digits_hexadecimal_max", "zero bound no used",
                             string_digits_hexadecimal_max(null, 0, null), 0);
                        got_used = (positive)-1;
                        same("string_digits_hexadecimal_escape_max",
                             "zero bound null value",
                             string_digits_hexadecimal_escape_max(
                                 null, 0, address_of got_used), 0);
                        same("string_digits_hexadecimal_escape_max",
                             "zero bound null used", got_used, 0);
                        same("string_digits_hexadecimal_escape_max",
                             "zero bound no used",
                             string_digits_hexadecimal_escape_max(null, 0, null),
                             0);
                }
        }

        for (positive i = 0; i < sizeof(invalid_bases) / sizeof(invalid_bases[0]); i++)
        {
                positive got_used = (positive)-1;

                same("string_digits_base_max", "invalid base null value",
                     string_digits_base_max(null, 1, invalid_bases[i],
                                            address_of got_used),
                     0);
                same("string_digits_base_max", "invalid base null used", got_used, 0);
        }

        for (positive base = 2; base <= 36; base++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive length = 0; length <= 24; length++)
                        {
                                p8 address_to subject = span_subject(offset);

                                reference_fill(span_room, 0xa5, sizeof(span_room));

                                for (positive i = 0; i < length; i++)
                                {
                                        positive digit = (i * 17 + base + offset) % base;

                                        subject[i] =
                                            base_digit_character(digit, (i & 1) != 0);
                                }

                                // A valid byte beyond the fence makes any
                                // over-read continue instead of hiding itself.
                                subject[length + 1] = '0';
                                subject[length + 2] = '0';

                                for (positive byte = 0; byte < 256; byte++)
                                {
                                        positive got_used = (positive)-1;
                                        positive want_used = (positive)-1;

                                        subject[length] = (p8)byte;

                                        same("string_digits_base_max", "every byte value",
                                             string_digits_base_max(
                                                 subject, length + 1, base,
                                                 address_of got_used),
                                             reference_digits_base_max(
                                                 subject, length + 1, base,
                                                 address_of want_used));
                                        same("string_digits_base_max", "every byte used",
                                             got_used, want_used);

                                        if (base == 8)
                                        {
                                                got_used = (positive)-1;
                                                same("string_digits_octal_max",
                                                     "every byte value",
                                                     string_digits_octal_max(
                                                         subject, length + 1,
                                                         address_of got_used),
                                                     reference_digits_base_max(
                                                         subject, length + 1, 8,
                                                         address_of want_used));
                                                same("string_digits_octal_max",
                                                     "every byte used", got_used,
                                                     want_used);
                                                got_used = (positive)-1;
                                                same("string_digits_octal_escape_max",
                                                     "every byte value",
                                                     string_digits_octal_escape_max(
                                                         subject, length + 1,
                                                         address_of got_used),
                                                     reference_digits_base_max(
                                                         subject, length + 1, 8,
                                                         address_of want_used));
                                                same("string_digits_octal_escape_max",
                                                     "every byte used", got_used,
                                                     want_used);
                                        }
                                        else if (base == 16)
                                        {
                                                got_used = (positive)-1;
                                                same("string_digits_hexadecimal_max",
                                                     "every byte value",
                                                     string_digits_hexadecimal_max(
                                                         subject, length + 1,
                                                         address_of got_used),
                                                     reference_digits_base_max(
                                                         subject, length + 1, 16,
                                                         address_of want_used));
                                                same("string_digits_hexadecimal_max",
                                                     "every byte used", got_used,
                                                     want_used);
                                                got_used = (positive)-1;
                                                same("string_digits_hexadecimal_escape_max",
                                                     "every byte value",
                                                     string_digits_hexadecimal_escape_max(
                                                         subject, length + 1,
                                                         address_of got_used),
                                                     reference_digits_base_max(
                                                         subject, length + 1, 16,
                                                         address_of want_used));
                                                same("string_digits_hexadecimal_escape_max",
                                                     "every byte used", got_used,
                                                     want_used);
                                        }
                                }

                                same("string_digits_base_max", "optional used",
                                     string_digits_base_max(subject, length, base, null),
                                     reference_digits_base_max(subject, length, base,
                                                               null));

                                if (base == 8)
                                {
                                        same("string_digits_octal_max", "optional used",
                                             string_digits_octal_max(subject, length,
                                                                     null),
                                             reference_digits_base_max(subject,
                                                                       length, 8,
                                                                       null));
                                        same("string_digits_octal_escape_max",
                                             "optional used",
                                             string_digits_octal_escape_max(subject,
                                                                            length,
                                                                            null),
                                             reference_digits_base_max(subject,
                                                                       length, 8,
                                                                       null));
                                }
                                else if (base == 16)
                                {
                                        same("string_digits_hexadecimal_max",
                                             "optional used",
                                             string_digits_hexadecimal_max(subject,
                                                                           length,
                                                                           null),
                                             reference_digits_base_max(subject,
                                                                       length, 16,
                                                                       null));
                                        same("string_digits_hexadecimal_escape_max",
                                             "optional used",
                                             string_digits_hexadecimal_escape_max(
                                                 subject, length, null),
                                             reference_digits_base_max(subject,
                                                                       length, 16,
                                                                       null));
                                }
                        }

        // Ninety six binary digits are enough to overflow a 64-bit word; the
        // same run therefore crosses the wrap point in every larger base too.
        for (positive base = 2; base <= 36; base++)
                for (positive offset = 0; offset < 16; offset++)
                {
                        p8 address_to subject = span_subject(offset);

                        reference_fill(span_room, 0xa5, sizeof(span_room));

                        for (positive i = 0; i < 98; i++)
                        {
                                positive digit = (i * 29 + base - 1) % base;

                                subject[i] = base_digit_character(digit, (i & 1) != 0);
                        }

                        for (positive bound = 0; bound <= 96; bound++)
                        {
                                positive got_used = (positive)-1;
                                positive want_used = (positive)-1;

                                same("string_digits_base_max", "bounded wrap value",
                                     string_digits_base_max(subject, bound, base,
                                                            address_of got_used),
                                     reference_digits_base_max(subject, bound, base,
                                                               address_of want_used));
                                same("string_digits_base_max", "bounded wrap used",
                                     got_used, want_used);

                                if (base == 8)
                                {
                                        got_used = (positive)-1;
                                        same("string_digits_octal_max",
                                             "bounded wrap value",
                                             string_digits_octal_max(subject, bound,
                                                                     address_of got_used),
                                             reference_digits_base_max(
                                                 subject, bound, 8,
                                                 address_of want_used));
                                        same("string_digits_octal_max",
                                             "bounded wrap used", got_used,
                                             want_used);
                                        got_used = (positive)-1;
                                        same("string_digits_octal_escape_max",
                                             "bounded wrap value",
                                             string_digits_octal_escape_max(
                                                 subject, bound,
                                                 address_of got_used),
                                             reference_digits_base_max(
                                                 subject, bound, 8,
                                                 address_of want_used));
                                        same("string_digits_octal_escape_max",
                                             "bounded wrap used", got_used,
                                             want_used);
                                }
                                else if (base == 16)
                                {
                                        got_used = (positive)-1;
                                        same("string_digits_hexadecimal_max",
                                             "bounded wrap value",
                                             string_digits_hexadecimal_max(
                                                 subject, bound,
                                                 address_of got_used),
                                             reference_digits_base_max(
                                                 subject, bound, 16,
                                                 address_of want_used));
                                        same("string_digits_hexadecimal_max",
                                             "bounded wrap used", got_used,
                                             want_used);
                                        got_used = (positive)-1;
                                        same("string_digits_hexadecimal_escape_max",
                                             "bounded wrap value",
                                             string_digits_hexadecimal_escape_max(
                                                 subject, bound,
                                                 address_of got_used),
                                             reference_digits_base_max(
                                                 subject, bound, 16,
                                                 address_of want_used));
                                        same("string_digits_hexadecimal_escape_max",
                                             "bounded wrap used", got_used,
                                             want_used);
                                }
                        }
                }
}

/*
        The numbers where the arithmetic is the question rather than the scan:
        the largest a positive holds, the one past it that wraps, and a run of
        leading zeros long enough that a routine keeping the place value in a
        register would have run out of them.

        Copied into the buffer rather than passed as the literal they are
        written as, for the reason the block above gives.
*/
static string_address digit_exacts[] = {
    "0",
    "00",
    "007",
    "9",
    "10",
    "0000000000000000000000000001",
    "18446744073709551615",
    "18446744073709551616",
    "18446744073709551625",
    "99999999999999999999",
    "999999999999999999999999999999",
    "4294967295",
    "4294967296",
    "1234567890123456789",
};

#define DIGIT_EXACT_COUNT (sizeof(digit_exacts) / sizeof(digit_exacts[0]))

fn check_digits_exact()
{
        for (positive which = 0; which < DIGIT_EXACT_COUNT; which++)
                for (positive br = 0; br < DIGIT_BREAKER_COUNT; br++)
                        for (positive offset = 0; offset < 8; offset++)
                        {
                                p8 address_to subject = span_subject(offset);
                                positive length = string_length(digit_exacts[which]);
                                positive want_used = 0;
                                positive got_used = 0;

                                reference_fill(span_room, '7', sizeof(span_room));

                                for (positive i = 0; i < length; i++)
                                        subject[i] = digit_exacts[which][i];

                                subject[length] = digit_breakers[br];

                                same("string_digits", "the exact ones",
                                     string_digits(subject, address_of got_used),
                                     reference_digits(subject, (positive)-1,
                                                      address_of want_used));

                                same("string_digits", "the exact ones, taken",
                                     got_used, want_used);

                                // Includes positive_max, the first value past
                                // it and longer all-digit words. Validity is
                                // independent of wrap, while the stored value
                                // must wrap exactly like the prefix parser.
                                {
                                        positive got_value =
                                            (positive)0x5a5a5a5a5a5a5a5aull;
                                        positive want_value = got_value;

                                        same("string_digits_exact", "wrap validity",
                                             string_digits_exact(
                                                 subject, address_of got_value),
                                             reference_digits_exact(
                                                 subject, address_of want_value));
                                        same("string_digits_exact", "wrapped value",
                                             got_value, want_value);
                                }

                                for (positive bound = 0; bound <= length + 1; bound++)
                                        same("string_digits_max", "the exact ones",
                                             string_digits_max(subject, bound, 0),
                                             reference_digits(subject, bound, 0));
                        }
}

/*
        The digits of a number, into a buffer.

        The reference is the loop this replaced, written out the way all
        twenty two copies of it were: least significant digit first into a
        scratch array, then the array backwards into the output.

        Two things are checked that a value comparison would miss. The buffer
        is filled with a byte that is not a digit and the bytes on both sides
        of the answer are checked afterwards, because a routine that writes a
        terminator, or pads to a width, or writes the scratch out whole,
        answers the right length and corrupts what was next to it. And the
        destination is walked across eight offsets, so a store that assumed an
        aligned buffer is caught.
*/
positive reference_into(p8 address_to into, positive value)
{
        p8 scratch[24];
        positive have = 0;

        if (!value)
                scratch[have++] = '0';

        while (value)
        {
                scratch[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        for (positive i = 0; i < have; i++)
                into[i] = scratch[have - 1 - i];

        return have;
}

/*
        The compact binary spelling that used to be repeated in file_human,
        df_amount and ls_human_width. This reference says the intended
        overflow-safe rule explicitly: a ceiling is quotient plus a nonzero
        remainder, never an addition near positive_max.
*/
positive reference_into_human_1024(p8 address_to into, positive value)
{
        static p8 units[] = "BKMGTPE";
        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 6)
        {
                divisor *= 1024;
                unit++;
        }

        if (!unit)
        {
                positive length = reference_into(into, value);

                into[length] = end;
                return length;
        }

        positive quotient = value / divisor;
        positive remainder = value % divisor;
        positive length;

        if (quotient >= 10)
                length = reference_into(into, quotient + (remainder != 0));
        else
        {
                positive fraction = (remainder * 10 + divisor - 1) / divisor;

                if (fraction == 10)
                {
                        quotient++;
                        fraction = 0;
                }

                if (quotient >= 10)
                        length = reference_into(into, quotient);
                else
                {
                        length = reference_into(into, quotient);
                        into[length++] = '.';
                        length += reference_into(into + length, fraction);
                }
        }

        into[length++] = units[unit];
        into[length] = end;

        return length;
}

positive reference_into_base(p8 address_to into, positive value, positive base,
                             bool upper)
{
        p8 scratch[64];
        positive have = 0;

        do
        {
                positive digit = value % base;

                scratch[have++] =
                    (p8)(digit < 10 ? '0' + digit
                                    : (upper ? 'A' : 'a') + digit - 10);
                value /= base;
        } while (value);

        for (positive i = 0; i < have; i++)
                into[i] = scratch[have - 1 - i];

        return have;
}

static positive into_exacts[] = {
    0, 1, 2, 9, 10, 11, 99, 100, 101, 999, 1000, 1001,
    9999, 10000, 65535, 65536, 99999, 100000, 999999, 1000000,
    99999999ull, 100000000ull, 100000001ull,
    999999999ull, 1000000000ull,
    4294967295ull, 4294967296ull,
    9999999999ull, 10000000000ull,
    999999999999999999ull, 1000000000000000000ull,
    9999999999999999999ull, 10000000000000000000ull,
    18446744073709551615ull,
};

#define INTO_EXACT_COUNT (sizeof(into_exacts) / sizeof(into_exacts[0]))

fn check_into_one(positive value, positive offset, p8 guard)
{
        p8 address_to got;
        p8 address_to want;
        positive got_length;
        positive want_length;

        reference_fill(span_room, guard, sizeof(span_room));
        reference_fill(field, guard, sizeof(field));

        got = span_subject(offset);
        want = (p8 address_to)aligned_at(field) + SPAN_HEAD + offset;

        got_length = positive_into(got, value);
        want_length = reference_into(want, value);

        same("positive_into", "how many", got_length, want_length);
        same_bytes("positive_into", "the digits", got, want, want_length);

        // Nothing in front of the answer and nothing behind it. A routine
        // that terminates the string, or hands back the whole scratch, gets
        // the length right and is still wrong.
        for (positive back = 1; back <= 8; back++)
                same("positive_into", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 0; after < 8; after++)
                same("positive_into", "behind it",
                     (positive)got[got_length + after], (positive)guard);

        reference_fill(span_room, guard, sizeof(span_room));
        reference_fill(field, guard, sizeof(field));

        got = span_subject(offset);
        want = (p8 address_to)aligned_at(field) + SPAN_HEAD + offset;

        got_length = positive_into_string(got, value);
        want_length = reference_into(want, value);
        want[want_length] = end;

        same("positive_into_string", "how many", got_length, want_length);
        same_bytes("positive_into_string", "the string",
                   got, want, want_length + 1);

        for (positive back = 1; back <= 8; back++)
                same("positive_into_string", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 1; after <= 8; after++)
                same("positive_into_string", "behind it",
                     (positive)got[got_length + after], (positive)guard);
}

fn check_bipolar_into_one(bipolar value, positive offset, p8 guard)
{
        p8 address_to got;
        p8 address_to want;
        positive got_length;
        positive want_length = 0;

        reference_fill(span_room, guard, sizeof(span_room));
        reference_fill(field, guard, sizeof(field));

        got = span_subject(offset);
        want = (p8 address_to)aligned_at(field) + SPAN_HEAD + offset;

        if (value < 0)
                want[want_length++] = '-';

        want_length += reference_into(want + want_length,
                                      value < 0
                                          ? (positive)0 - (positive)value
                                          : (positive)value);

        got_length = bipolar_into(got, value);

        same("bipolar_into", "how many", got_length, want_length);
        same_bytes("bipolar_into", "the field", got, want, want_length);

        for (positive back = 1; back <= 8; back++)
                same("bipolar_into", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 0; after < 8; after++)
                same("bipolar_into", "behind it",
                     (positive)got[got_length + after], (positive)guard);

        reference_fill(span_room, guard, sizeof(span_room));
        got = span_subject(offset);
        want[want_length] = end;

        got_length = bipolar_into_string(got, value);

        same("bipolar_into_string", "how many", got_length, want_length);
        same_bytes("bipolar_into_string", "the string",
                   got, want, want_length + 1);

        for (positive back = 1; back <= 8; back++)
                same("bipolar_into_string", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 1; after <= 8; after++)
                same("bipolar_into_string", "behind it",
                     (positive)got[got_length + after], (positive)guard);
}

fn check_into()
{
        // A digit, a byte that is not one, a zero and a high byte: the four
        // ways a stray write shows up as something plausible.
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};

        for (positive g = 0; g < sizeof(guards); g++)
                for (positive offset = 0; offset < 8; offset++)
                {
                        for (positive e = 0; e < INTO_EXACT_COUNT; e++)
                                check_into_one(into_exacts[e], offset, guards[g]);

                        // And every power of ten either side, so each length
                        // the divide loop can produce is reached.
                        positive power = 1;

                        for (positive d = 0; d < 19; d++)
                        {
                                check_into_one(power - 1, offset, guards[g]);
                                check_into_one(power, offset, guards[g]);
                                check_into_one(power + 1, offset, guards[g]);
                                power *= 10;
                        }

                        for (positive r = 0; r < 24; r++)
                                check_into_one(next(), offset, guards[g]);

                        /*
                                The minimum signed value negates to its own
                                bits. These exercise both signed entry points,
                                every destination alignment, and the bytes on
                                either side of their contracts.
                        */
                        static bipolar values[] = {
                            bipolar_min, bipolar_min + 1, -1,
                            0, 1, bipolar_max,
                        };

                        for (positive i = 0;
                             i < sizeof(values) / sizeof(values[0]); i++)
                                check_bipolar_into_one(values[i], offset, guards[g]);
                }

        /*
                The direct table path has a finite domain, so cover all of it
                rather than sampling it: every unsigned value through 9999,
                every negative magnitude through 9999, and 10000 as the first
                value handed to the shared wide core. Alignment and guard
                rotate with the value, exercising every byte position without
                multiplying this sweep by another forty identical passes.
        */
        for (positive value = 0; value <= 10000; value++)
        {
                positive offset = value & 7;
                p8 guard = guards[value % (sizeof(guards) / sizeof(guards[0]))];

                check_into_one(value, offset, guard);
                if (value)
                        check_bipolar_into_one(-(bipolar)value, offset, guard);
        }
}

/*
        Human sizes have two contracts: the terminated bytes and the writer
        protocol. The old file helper sent an unscaled number as one digit
        run, an integer-scaled number as digits then suffix, and a fractional
        one as four single-byte calls. Capture both so folding through a
        buffer cannot silently coalesce a writer that observes boundaries.
*/
static p8 human_capture[16];
static positive human_used;
static positive human_calls;
static positive human_call_lengths[4];
static p8 human_call_first[4];
static bool human_overflow;

fn human_writer(address_any data, positive length)
{
        p8 address_to bytes = (p8 address_to)data;

        if (human_calls < sizeof(human_call_lengths) /
                          sizeof(human_call_lengths[0]))
        {
                human_call_lengths[human_calls] = length;
                human_call_first[human_calls] = length ? bytes[0] : 0;
        }
        else
                human_overflow = true;

        human_calls++;

        if (length > sizeof(human_capture) - min(human_used,
                                                  sizeof(human_capture)))
        {
                human_overflow = true;
                return;
        }

        reference_copy(human_capture + human_used, bytes, length);
        human_used += length;
}

fn check_human_one(positive value, positive offset, p8 guard)
{
        reference_fill(span_room, guard, sizeof(span_room));
        reference_fill(field, guard, sizeof(field));

        p8 address_to got = span_subject(offset);
        p8 address_to want =
            (p8 address_to)aligned_at(field) + SPAN_HEAD + offset;
        positive got_length = positive_into_human_1024_string(got, value);
        positive want_length = reference_into_human_1024(want, value);

        same("positive_into_human_1024_string", "how many", got_length, want_length);
        same_bytes("positive_into_human_1024_string", "the terminated bytes",
                   got, want, want_length + 1);

        for (positive back = 1; back <= 8; back++)
                same("positive_into_human_1024_string", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 1; after <= 8; after++)
                same("positive_into_human_1024_string", "behind the terminator",
                     (positive)got[got_length + after], (positive)guard);

        reference_fill(human_capture, guard, sizeof(human_capture));
        human_used = 0;
        human_calls = 0;
        human_overflow = false;

        positive_to_human_1024(human_writer, value);

        same("positive_to_human_1024", "did not exceed the capture",
             human_overflow, false);
        same("positive_to_human_1024", "how many bytes",
             human_used, want_length);
        same_bytes("positive_to_human_1024", "the bytes",
                   human_capture, want, want_length);

        bool fractional = want_length == 4 && want[1] == '.';
        bool scaled = want[want_length - 1] > '9';
        positive expected_calls = fractional ? 4 : (scaled ? 2 : 1);

        same("positive_to_human_1024", "how many writer calls",
             human_calls, expected_calls);

        if (fractional)
                for (positive call = 0; call < 4 && call < human_calls; call++)
                {
                        same("positive_to_human_1024",
                             "one byte in each fractional call",
                             human_call_lengths[call], 1);
                        same("positive_to_human_1024",
                             "fractional call order",
                             human_call_first[call], want[call]);
                }
        else if (scaled && human_calls >= 2)
        {
                same("positive_to_human_1024", "integer digit run",
                     human_call_lengths[0], want_length - 1);
                same("positive_to_human_1024", "integer first digit",
                     human_call_first[0], want[0]);
                same("positive_to_human_1024", "integer suffix length",
                     human_call_lengths[1], 1);
                same("positive_to_human_1024", "integer suffix byte",
                     human_call_first[1], want[want_length - 1]);
        }
        else if (human_calls)
        {
                same("positive_to_human_1024", "plain digit run",
                     human_call_lengths[0], want_length);
                same("positive_to_human_1024", "plain first digit",
                     human_call_first[0], want[0]);
        }
}

fn check_human_1024()
{
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};
        positive maximum = ~(positive)0;

        // Complete finite low domain, rotating every alignment and poison.
        for (positive value = 0; value <= 65535; value++)
                check_human_one(value, value & 15,
                                guards[value % sizeof(guards)]);

        positive divisor = 1024;

        for (positive unit = 1; unit <= 6; unit++)
        {
                check_human_one(divisor - 1, unit, guards[unit % sizeof(guards)]);
                check_human_one(divisor, unit + 1,
                                guards[(unit + 1) % sizeof(guards)]);
                check_human_one(divisor + 1, unit + 2,
                                guards[(unit + 2) % sizeof(guards)]);

                // The presentation changes at nine plus any remainder, and
                // the unit changes only at the next exact power.
                static positive multipliers[] = {9, 10};

                for (positive m = 0;
                     m < sizeof(multipliers) / sizeof(multipliers[0]); m++)
                {
                        positive edge = multipliers[m] * divisor;

                        check_human_one(edge - 1, m + unit,
                                        guards[(m + unit) % sizeof(guards)]);
                        check_human_one(edge, m + unit + 1,
                                        guards[(m + unit + 1) % sizeof(guards)]);
                        check_human_one(edge + 1, m + unit + 2,
                                        guards[(m + unit + 2) % sizeof(guards)]);
                }

                // Every tenth boundary from either side, at both ends of the
                // one-decimal range. Powers of two are not divisible by ten,
                // so the floor and its neighbours exercise the ceiling.
                for (positive tenth = 1; tenth < 10; tenth++)
                {
                        positive remainder = (tenth * divisor) / 10;

                        for (positive quotient = 1; quotient <= 8; quotient += 7)
                        {
                                positive edge = quotient * divisor + remainder;

                                check_human_one(edge - 1, tenth,
                                                guards[tenth % sizeof(guards)]);
                                check_human_one(edge, tenth + 1,
                                                guards[(tenth + 1) % sizeof(guards)]);
                                check_human_one(edge + 1, tenth + 2,
                                                guards[(tenth + 2) % sizeof(guards)]);
                        }
                }

                if (divisor <= maximum / 1024)
                        divisor *= 1024;
        }

        // The exact range where the former numerator wrapped, plus the whole
        // u64 ceiling. These must say 15E, 16E, 16E rather than 15E, 0E, 0E.
        divisor = (positive)1 << 60;
        check_human_one(15 * divisor, 13, 0xa5);
        check_human_one(15 * divisor + 1, 14, 0x5a);
        check_human_one(maximum - 1, 15, 0xff);
        check_human_one(maximum, 0, 0x00);

        for (positive i = 0; i < 65536; i++)
                check_human_one(next(), i & 15,
                                guards[(i >> 4) % sizeof(guards)]);
}

#include "verify_human_nearest.inc"

static b32 reference_wait_status_code_base(positive raw, b32 signal_base)
{
        positive signal = raw & 0x7f;

        if (!signal)
                return (b32)((raw >> 8) & 0xff);

        b32 answer = signal_base + (b32)signal;

        if ((raw & 0x80) && (signal_base & 0x100))
                answer += 0x100;

        return answer;
}

/*
        Every kernel-owned status word under each public policy.  Core is
        deliberately crossed here rather than tested with only one signal:
        base zero and the shell's 128 ignore bit seven, while gawk's base 256
        reports it as another 256.  Repeating the complete low word under
        hostile high-bit masks proves no caller garbage reaches the answer.
*/
fn check_wait_status_code_base()
{
        static const b32 bases[] = {0, 128, 256};
        static const positive high_bits[] = {
            0x0000000100000000ull,
            0x8000000000000000ull,
            0xffffffffffff0000ull,
        };

        for (positive b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
                for (positive low = 0; low <= 65535; low++)
                {
                        b32 want = reference_wait_status_code_base(low, bases[b]);

                        same("wait_status_code_base", "every raw 16-bit status",
                             (positive)wait_status_code_base(low, bases[b]),
                             (positive)want);

                        for (positive h = 0;
                             h < sizeof(high_bits) / sizeof(high_bits[0]); h++)
                        {
                                positive raw = low | high_bits[h];

                                same("wait_status_code_base", "ignores every high-bit mask",
                                     (positive)wait_status_code_base(raw, bases[b]),
                                     (positive)want);
                        }
                }
}

/*
        A padded field is a writer protocol, not merely a byte result: every
        leading byte is its own call, the optional prefix is its own call, and
        all digits are the final call. These records prove that boundary and
        order while the guarded destination proves the bytes. Widths through
        forty cross every decimal length, every destination alignment, both
        padding alphabets, disabled padding, and absent/present prefixes. The
        finite 0..100000 converter domain is swept once more with those axes
        rotating, then deliberately large widths prove there is no
        width-sized stack allocation or hidden truncation.
*/
#define PADDED_ROOM 1280
#define PADDED_CALL_MAX 1100

static p8 padded_got_room[PADDED_ROOM];
static p8 padded_want_room[PADDED_ROOM];
static p8 address_to padded_output;
static positive padded_capacity;
static positive padded_used;
static positive padded_calls;
static positive padded_call_lengths[PADDED_CALL_MAX];
static p8 padded_call_first[PADDED_CALL_MAX];
static bool padded_overflow;

fn padded_writer(address_any data, positive length)
{
        p8 address_to bytes = (p8 address_to)data;

        if (padded_calls < PADDED_CALL_MAX)
        {
                padded_call_lengths[padded_calls] = length;
                padded_call_first[padded_calls] = length ? bytes[0] : 0;
        }
        else
                padded_overflow = true;

        padded_calls++;

        if (length > padded_capacity - min(padded_used, padded_capacity))
        {
                padded_overflow = true;
                return;
        }

        reference_copy(padded_output + padded_used, bytes, length);
        padded_used += length;
}

/*
        writer_field is a callback protocol as much as a byte operation.  The
        capture records every boundary and its address, while the assembly
        shim deliberately destroys every general caller-saved register after
        the C capture returns.  A field implementation that keeps its body,
        count, pad, or writer in scratch registers cannot pass by accident.
*/
#define FIELD_ROOM 512
#define FIELD_CALL_MAX 320

static p8 field_room[FIELD_ROOM];
static p8 field_body_room[128];
static p8 field_string_room[128];
static p8 field_body_before[128];
static p8 address_to field_output;
static positive field_capacity;
static positive field_used;
static positive field_calls;
static positive field_call_lengths[FIELD_CALL_MAX];
static p8 field_call_first[FIELD_CALL_MAX];
static address_any field_call_data[FIELD_CALL_MAX];
static bool field_overflow;

__attribute__((noinline, noclone, used))
fn field_capture(address_any data, positive length)
{
        p8 address_to bytes = (p8 address_to)data;

        if (field_calls < FIELD_CALL_MAX)
        {
                field_call_lengths[field_calls] = length;
                field_call_first[field_calls] = length ? bytes[0] : 0;
                field_call_data[field_calls] = data;
        }
        else
                field_overflow = true;

        field_calls++;

        if (!length || length > field_capacity - min(field_used, field_capacity))
        {
                field_overflow = true;
                return;
        }

        reference_copy(field_output + field_used, bytes, length);
        field_used += length;
}

fn field_writer(address_any data, positive length);

#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(field_writer)
    "sub $8, %rsp\n   call field_capture\n   add $8, %rsp\n"
    "mov $1, %eax\n   mov $2, %ecx\n   mov $3, %edx\n"
    "mov $4, %esi\n   mov $5, %edi\n   mov $6, %r8d\n"
    "mov $7, %r9d\n   mov $8, %r10d\n   mov $9, %r11d\n"
    ASM_RET
    ASM_END(field_writer));
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(field_writer)
    "stp x29, x30, [sp, #-16]!\n   mov x29, sp\n   bl field_capture\n"
    "ldp x29, x30, [sp], #16\n"
    "mov x0, #1\n   mov x1, #2\n   mov x2, #3\n   mov x3, #4\n"
    "mov x4, #5\n   mov x5, #6\n   mov x6, #7\n   mov x7, #8\n"
    "mov x8, #9\n   mov x9, #10\n   mov x10, #11\n   mov x11, #12\n"
    "mov x12, #13\n   mov x13, #14\n   mov x14, #15\n   mov x15, #16\n"
    "mov x16, #17\n   mov x17, #18\n   mov x18, #19\n"
    ASM_RET
    ASM_END(field_writer));
#else
__asm__(
    ASM_SECTION
    ASM_FUNC(field_writer)
    "addi sp, sp, -16\n   sd ra, 8(sp)\n   call field_capture\n"
    "ld ra, 8(sp)\n   addi sp, sp, 16\n"
    "li t0, 1\n   li t1, 2\n   li t2, 3\n   li t3, 4\n"
    "li t4, 5\n   li t5, 6\n   li t6, 7\n"
    "li a0, 8\n   li a1, 9\n   li a2, 10\n   li a3, 11\n"
    "li a4, 12\n   li a5, 13\n   li a6, 14\n   li a7, 15\n"
    ASM_RET
    ASM_END(field_writer));
#endif

fn check_field_case(string_address name, address_any data, positive length,
                    positive width, p8 pad, bool left, positive output_residue,
                    bool string_form)
{
        positive padding = width > length ? width - length : 0;
        positive total = length + padding;
        positive body_call = left ? 0 : padding;
        p8 address_to bytes = (p8 address_to)data;

        if (length)
                reference_copy(field_body_before, data, length);

        reference_fill(field_room, 0xa5, sizeof(field_room));
        field_output = field_room + 16 + output_residue;
        field_capacity = total;
        field_used = field_calls = 0;
        field_overflow = false;

        if (string_form)
                string_to_field(field_writer, (string_address)data,
                                width, (b8)pad, left);
        else
                writer_field(field_writer, data, length,
                             width, (b8)pad, left);

        same(name, "capture stayed in bounds", field_overflow, false);
        same(name, "total byte count", field_used, total);
        same(name, "exact callback count", field_calls,
             padding + (length ? 1 : 0));

        for (positive guard = 1; guard <= 8; guard++)
        {
                same(name, "guard before output",
                     field_output[-(bipolar)guard], 0xa5);
                same(name, "guard after output",
                     field_output[total + guard - 1], 0xa5);
        }

        for (positive at = 0; at < total; at++)
        {
                bool in_body = left ? at < length : at >= padding;
                positive body_at = left ? at : at - padding;
                p8 want = in_body ? field_body_before[body_at] : pad;

                same(name, "byte order", field_output[at], want);
        }

        for (positive call = 0; call < field_calls && call < FIELD_CALL_MAX;
             call++)
        {
                bool is_body = length && call == body_call;

                same(name, "callback length", field_call_lengths[call],
                     is_body ? length : 1);
                same(name, "callback first byte", field_call_first[call],
                     is_body ? field_body_before[0] : pad);

                if (is_body)
                        same(name, "body callback address",
                             (positive)field_call_data[call], (positive)data);
        }

        if (length)
                same_bytes(name, "input remains unchanged",
                           (b8 address_to)data,
                           (b8 address_to)field_body_before, length);
}

fn check_writer_fields()
{
        // Counted input is deliberately binary and not NUL terminated. Every
        // small length, minimum width, pointer residue, alignment direction,
        // and a rotating full-byte pad reaches the hostile callback.
        for (positive length = 0; length <= 64; length++)
                for (positive width = 0; width <= 64; width++)
                        for (positive residue = 0; residue < 16; residue++)
                                for (positive left = 0; left < 2; left++)
                                {
                                        p8 address_to data =
                                            field_body_room + 16 + residue;
                                        p8 pad = (p8)(length * 67 + width * 29 +
                                                      residue * 17 + left * 131);

                                        reference_fill(field_body_room, 0x7d,
                                                       sizeof(field_body_room));
                                        for (positive i = 0; i < length; i++)
                                                data[i] = (p8)(i * 43 + length * 11 + 1);
                                        if (length > 2)
                                                data[1] = 0;
                                        data[length] = 0x7e;

                                        check_field_case(
                                            "writer_field", length ? data :
                                                (address_any)(positive)1,
                                            length, width, pad, (bool)left,
                                            (length * 5 + width * 3 + residue) & 15,
                                            false);
                                }

        // Every possible padding byte, explicitly including NUL and both
        // halves of the signed-char range.
        for (positive pad = 0; pad <= 255; pad++)
        {
                p8 address_to data = field_body_room + 16 + (pad & 15);

                data[0] = 0;
                data[1] = 0x80;
                data[2] = 0xff;
                data[3] = 0x7e;
                check_field_case("writer_field pad byte", data, 3, 9,
                                 (p8)pad, (bool)(pad >> 7), pad & 15, false);
        }

        // The string wrapper crosses the complete small length/width plane;
        // rotating residues and pad values keeps this focused on measuring
        // once and routing the identical core rather than repeating the much
        // larger raw-field alignment matrix above.
        for (positive length = 0; length <= 64; length++)
                for (positive width = 0; width <= 64; width++)
                        for (positive left = 0; left < 2; left++)
                        {
                                positive residue =
                                    (length * 7 + width * 11 + left) & 15;
                                p8 address_to text =
                                    field_string_room + 16 + residue;
                                p8 pad = (p8)(length * 31 + width * 73 + left);

                                reference_fill(field_string_room, 0x6d,
                                               sizeof(field_string_room));
                                for (positive i = 0; i < length; i++)
                                        text[i] = (p8)(1 + (i * 47 + length) % 255);
                                text[length] = end;

                                check_field_case("string_to_field", text, length,
                                                 width, pad, (bool)left,
                                                 (residue * 9 + width) & 15, true);
                        }

        // Wider than the exhaustive plane, while remaining below the fixed
        // transcript capacity. This rejects a small-field-only counter.
        p8 address_to data = field_body_room + 19;
        data[0] = 0x80;
        data[1] = 0;
        data[2] = 0xff;
        check_field_case("writer_field wide", data, 3, 257, 0,
                         false, 15, false);
        check_field_case("writer_field wide", data, 3, 257, 0xff,
                         true, 0, false);
}

fn check_base_field()
{
        static positive values[] = {0, 1, 7, 8, 9, 15, 16, 255,
                                    0xffffffffffffffffull};
        static positive widths[] = {0, 1, 2, 5, 24, 257};
        static bipolar precisions[] = {-1, 0, 1, 4, 25};

        // writer_fill's observable contract is exactly one one-byte call per
        // requested byte, including large counts, and no call for zero.
        for (positive count = 0; count <= 257; count += count < 2 ? 1 : 255)
        {
                padded_output = padded_got_room;
                padded_capacity = sizeof(padded_got_room);
                padded_used = padded_calls = 0;
                padded_overflow = false;
                writer_fill(padded_writer, count, 0xa5);
                same("writer_fill", "capture", padded_overflow, false);
                same("writer_fill", "bytes", padded_used, count);
                same("writer_fill", "calls", padded_calls, count);
                for (positive i = 0; i < count; i++)
                {
                        same("writer_fill", "call length", padded_call_lengths[i], 1);
                        same("writer_fill", "call byte", padded_call_first[i], 0xa5);
                }
        }

        for (positive vi = 0; vi < sizeof(values) / sizeof(values[0]); vi++)
                for (positive base = 2; base <= 36; base++)
                        for (positive wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
                                for (positive pi = 0;
                                     pi < sizeof(precisions) / sizeof(precisions[0]); pi++)
                                        for (positive flags = 0; flags < 8; flags++)
                                        {
                                                p8 digits[72];
                                                positive upper = flags & 1;
                                                positive left = (flags >> 1) & 1;
                                                positive zero = (flags >> 2) & 1;
                                                positive sign = (vi & 1) ? '+' : 0;
                                                positive packed_prefix_length = vi % 4;
                                                positive prefix_length = min(packed_prefix_length, 2);
                                                positive style = sign | ((positive)'0' << 8) |
                                                    ((positive)'x' << 16) |
                                                    (packed_prefix_length << 24) | (upper << 26) |
                                                    (left << 27) | (zero << 28);
                                                positive length = reference_into_base(
                                                    digits, values[vi], base, upper);
                                                positive zeros = 0;
                                                bipolar precision = precisions[pi];
                                                positive head = (sign != 0) + prefix_length;

                                                if (precision >= 0 &&
                                                    (positive)precision > length)
                                                        zeros = (positive)precision - length;
                                                if (zero && !left && precision < 0 &&
                                                    widths[wi] > head + length)
                                                        zeros = widths[wi] - head - length;

                                                positive field = head + zeros + length;
                                                positive spaces = widths[wi] > field
                                                                      ? widths[wi] - field
                                                                      : 0;
                                                p8 want[400];
                                                positive made = 0;
                                                if (!left)
                                                        while (made < spaces)
                                                                want[made++] = ' ';
                                                if (sign)
                                                        want[made++] = (p8)sign;
                                                if (prefix_length)
                                                        want[made++] = '0';
                                                if (prefix_length == 2)
                                                        want[made++] = 'x';
                                                for (positive i = 0; i < zeros; i++)
                                                        want[made++] = '0';
                                                reference_copy(want + made, digits, length);
                                                made += length;
                                                if (left)
                                                        for (positive i = 0; i < spaces; i++)
                                                                want[made++] = ' ';

                                                padded_output = padded_got_room;
                                                padded_capacity = sizeof(padded_got_room);
                                                padded_used = padded_calls = 0;
                                                padded_overflow = false;
                                                positive_to_base_field(
                                                    padded_writer, values[vi], base,
                                                    widths[wi], precision, style);
                                                same("positive_to_base_field", "capture",
                                                     padded_overflow, false);
                                                same("positive_to_base_field", "bytes",
                                                     padded_used, made);
                                                same_bytes("positive_to_base_field", "field",
                                                           padded_got_room, want, made);
                                                positive call = 0;
                                                positive leading = left ? 0 : spaces;
                                                for (positive i = 0; i < leading; i++, call++)
                                                {
                                                        same("positive_to_base_field", "space call",
                                                             padded_call_lengths[call], 1);
                                                        same("positive_to_base_field", "space byte",
                                                             padded_call_first[call], ' ');
                                                }
                                                if (sign)
                                                {
                                                        same("positive_to_base_field", "sign call",
                                                             padded_call_lengths[call], 1);
                                                        same("positive_to_base_field", "sign byte",
                                                             padded_call_first[call++], sign);
                                                }
                                                if (prefix_length)
                                                {
                                                        same("positive_to_base_field", "prefix call",
                                                             padded_call_lengths[call], prefix_length);
                                                        same("positive_to_base_field", "prefix byte",
                                                             padded_call_first[call++], '0');
                                                }
                                                for (positive i = 0; i < zeros; i++, call++)
                                                {
                                                        same("positive_to_base_field", "zero call",
                                                             padded_call_lengths[call], 1);
                                                        same("positive_to_base_field", "zero byte",
                                                             padded_call_first[call], '0');
                                                }
                                                same("positive_to_base_field", "digit call",
                                                     padded_call_lengths[call], length);
                                                same("positive_to_base_field", "digit byte",
                                                     padded_call_first[call++], digits[0]);
                                                for (positive i = 0; i < (left ? spaces : 0);
                                                     i++, call++)
                                                        same("positive_to_base_field", "tail space call",
                                                             padded_call_lengths[call], 1);
                                                same("positive_to_base_field", "call count",
                                                     padded_calls, call);
                                        }

        static positive invalid[] = {0, 1, 37, (positive)-1};
        for (positive i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        {
                padded_output = padded_got_room;
                padded_capacity = sizeof(padded_got_room);
                padded_used = padded_calls = 0;
                padded_overflow = false;
                positive_to_base_field(padded_writer, 7, invalid[i], 20, 30,
                                       '+' | ((positive)3 << 24) |
                                           ((positive)1 << 28));
                same("positive_to_base_field", "invalid base calls", padded_calls, 0);
                same("positive_to_base_field", "invalid base bytes", padded_used, 0);
        }
}

fn check_padded_one(positive value, positive width, p8 pad, p8 prefix,
                    positive offset, p8 guard)
{
        reference_fill(padded_got_room, guard, sizeof(padded_got_room));
        reference_fill(padded_want_room, guard, sizeof(padded_want_room));

        p8 address_to got =
            (p8 address_to)aligned_at(padded_got_room) + 32 + offset;
        p8 address_to want =
            (p8 address_to)aligned_at(padded_want_room) + 32 + offset;
        p8 digits[24];
        positive digit_length = reference_into(digits, value);
        positive field_length = digit_length + (prefix != 0);
        positive padding = pad && width > field_length ? width - field_length : 0;
        positive wanted = 0;

        for (positive i = 0; i < padding; i++)
                want[wanted++] = pad;

        if (prefix)
                want[wanted++] = prefix;

        reference_copy(want + wanted, digits, digit_length);
        wanted += digit_length;

        padded_output = got;
        padded_capacity = sizeof(padded_got_room) -
                          (positive)(got - padded_got_room) - 8;
        padded_used = 0;
        padded_calls = 0;
        padded_overflow = false;

        positive_to_padded(padded_writer, value, width, pad, prefix);

        same("positive_to_padded", "did not exceed the capture",
             padded_overflow, false);
        same("positive_to_padded", "how many bytes", padded_used, wanted);
        same_bytes("positive_to_padded", "the field", got, want, wanted);

        positive expected_calls = padding + (prefix != 0) + 1;
        same("positive_to_padded", "how many writer calls",
             padded_calls, expected_calls);

        positive call = 0;

        for (; call < padding && call < padded_calls; call++)
        {
                same("positive_to_padded", "one byte in each pad call",
                     padded_call_lengths[call], 1);
                same("positive_to_padded", "the pad call byte",
                     padded_call_first[call], pad);
        }

        if (prefix && call < padded_calls)
        {
                same("positive_to_padded", "one byte in the prefix call",
                     padded_call_lengths[call], 1);
                same("positive_to_padded", "the prefix call byte",
                     padded_call_first[call], prefix);
                call++;
        }

        if (call < padded_calls)
        {
                same("positive_to_padded", "all digits in the final call",
                     padded_call_lengths[call], digit_length);
                same("positive_to_padded", "the first digit in the final call",
                     padded_call_first[call], digits[0]);
                call++;
        }

        same("positive_to_padded", "no calls after the digits",
             call, padded_calls);

        for (positive back = 1; back <= 8; back++)
                same("positive_to_padded", "in front of the field",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 0; after < 8; after++)
                same("positive_to_padded", "behind the field",
                     (positive)got[wanted + after], (positive)guard);
}

fn check_padded()
{
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};
        static p8 pads[] = {0, ' ', '0', '#'};
        static p8 prefixes[] = {0, '-', '+'};

        // Every low value, with every other finite axis rotating through it.
        for (positive value = 0; value <= 100000; value++)
                check_padded_one(
                    value, value % 41,
                    pads[(value / 41) % (sizeof(pads) / sizeof(pads[0]))],
                    prefixes[(value / 164) %
                             (sizeof(prefixes) / sizeof(prefixes[0]))],
                    value & 15,
                    guards[(value / 492) %
                           (sizeof(guards) / sizeof(guards[0]))]);

        // Full width/alignment/pad/prefix cross at every selected edge.
        for (positive e = 0; e < INTO_EXACT_COUNT; e++)
                for (positive width = 0; width <= 40; width++)
                        for (positive offset = 0; offset < 16; offset++)
                                for (positive p = 0;
                                     p < sizeof(pads) / sizeof(pads[0]); p++)
                                        for (positive x = 0;
                                             x < sizeof(prefixes) /
                                                     sizeof(prefixes[0]); x++)
                                                check_padded_one(
                                                    into_exacts[e], width,
                                                    pads[p], prefixes[x], offset,
                                                    guards[(e + width + offset + p + x) %
                                                           (sizeof(guards) /
                                                            sizeof(guards[0]))]);

        // Bounded but deliberately much wider than any in-tree field.
        static positive wide[] = {64, 127, 255, 1024};
        static positive wide_values[] = {0, 9, 10, positive_max};

        for (positive w = 0; w < sizeof(wide) / sizeof(wide[0]); w++)
                for (positive v = 0;
                     v < sizeof(wide_values) / sizeof(wide_values[0]); v++)
                        for (positive p = 0;
                             p < sizeof(pads) / sizeof(pads[0]); p++)
                                for (positive x = 0;
                                     x < sizeof(prefixes) / sizeof(prefixes[0]); x++)
                                        check_padded_one(
                                            wide_values[v], wide[w], pads[p],
                                            prefixes[x], (w + v + p + x) & 15,
                                            guards[(w + v + p + x) %
                                                   (sizeof(guards) /
                                                    sizeof(guards[0]))]);
}

/*
        The buffer form has a different contract from the writer form above:
        the complete field is contiguous and one return value owns its exact
        length. Cross small values with rotating widths first, then cross all
        integer length edges with every alignment, poison, pad and deliberately
        wide minimum. The guards prove no terminator, truncation or width-sized
        overrun; positive_max proves the twenty-digit move.
*/
#define INTO_PADDED_ROOM 1280

static p8 into_padded_got_room[INTO_PADDED_ROOM];
static p8 into_padded_want_room[INTO_PADDED_ROOM];

fn check_into_padded_one(positive value, positive width, p8 pad,
                         positive offset, p8 guard)
{
        reference_fill(into_padded_got_room, guard,
                       sizeof(into_padded_got_room));
        reference_fill(into_padded_want_room, guard,
                       sizeof(into_padded_want_room));

        p8 address_to got =
            (p8 address_to)aligned_at(into_padded_got_room) + 32 + offset;
        p8 address_to want =
            (p8 address_to)aligned_at(into_padded_want_room) + 32 + offset;
        p8 digits[24];
        positive digit_length = reference_into(digits, value);
        positive padding = pad && width > digit_length
                               ? width - digit_length
                               : 0;
        positive wanted = padding + digit_length;

        reference_fill(want, pad, padding);
        reference_copy(want + padding, digits, digit_length);

        positive length = positive_into_padded(got, value, width, pad);

        same("positive_into_padded", "how many", length, wanted);
        same_bytes("positive_into_padded", "the field", got, want, wanted);

        for (positive back = 1; back <= 8; back++)
                same("positive_into_padded", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 0; after < 8; after++)
                same("positive_into_padded", "behind it",
                     (positive)got[wanted + after], (positive)guard);
}

fn check_into_padded()
{
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};
        static p8 pads[] = {0, '0', ' ', '#'};
        static positive widths[] = {0, 1, 2, 6, 9, 19, 20,
                                    21, 32, 64, 127, 255, 1024};

        for (positive value = 0; value <= 100000; value++)
                check_into_padded_one(
                    value, value % 41,
                    pads[(value / 41) % (sizeof(pads) / sizeof(pads[0]))],
                    value & 15,
                    guards[(value / 164) %
                           (sizeof(guards) / sizeof(guards[0]))]);

        // Dense coverage of both bounded pair-emission lanes, including
        // values far above the exhaustive small sweep.
        for (positive r = 0; r < 10000; r++)
        {
                positive value6 = next() % 1000000;
                positive value9 = next() % 1000000000;

                check_into_padded_one(value6, 6, '0', r & 15,
                                      guards[r % sizeof(guards)]);
                check_into_padded_one(value9, 9, '0', (r + 7) & 15,
                                      guards[(r + 1) % sizeof(guards)]);
        }

        for (positive e = 0; e < INTO_EXACT_COUNT; e++)
                for (positive w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
                        for (positive offset = 0; offset < 16; offset++)
                                for (positive p = 0;
                                     p < sizeof(pads) / sizeof(pads[0]); p++)
                                        for (positive g = 0;
                                             g < sizeof(guards) / sizeof(guards[0]); g++)
                                                check_into_padded_one(
                                                    into_exacts[e], widths[w],
                                                    pads[p], offset, guards[g]);
}

fn check_into_pair()
{
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};
        p8 room[64];

        for (positive value = 0; value < 100; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive g = 0;
                             g < sizeof(guards) / sizeof(guards[0]); g++)
                        {
                                reference_fill(room, guards[g], sizeof(room));
                                p8 address_to got =
                                    (p8 address_to)aligned_at(room) + 16 + offset;
                                positive length = positive_into_pair(got, value);

                                same("positive_into_pair", "how many", length, 2);
                                same("positive_into_pair", "tens", got[0],
                                     '0' + value / 10);
                                same("positive_into_pair", "ones", got[1],
                                     '0' + value % 10);

                                for (positive back = 1; back <= 8; back++)
                                        same("positive_into_pair", "in front of it",
                                             *(got - back), guards[g]);
                                for (positive after = 0; after < 8; after++)
                                        same("positive_into_pair", "behind it",
                                             got[2 + after], guards[g]);
                        }
}

/*
        Every power fast path, every conversion base the tree uses, and the
        whole public 2..36 contract around it. Invalid bases return zero and
        leave the destination untouched.

        The 16-bit value domain is finite enough to exhaust for octal,
        decimal and hexadecimal. Alignment and poison rotate through that
        sweep, then every alignment/poison pair is crossed with zero,
        positive_max and either side of every power of each tree base. The
        remaining bases get both alphabets, their UINT64 edge, and a repeatable
        spread. Together those catch a wrong digit count, an uppercase leak,
        an assumed alignment and either kind of stray terminator/padding byte.
*/
static p8 base_got_room[128];
static p8 base_want_room[128];

fn check_into_base_one(positive value, positive base, bool upper,
                       positive offset, p8 guard)
{
        reference_fill(base_got_room, guard, sizeof(base_got_room));
        reference_fill(base_want_room, guard, sizeof(base_want_room));

        p8 address_to got = (p8 address_to)aligned_at(base_got_room) + 16 + offset;
        p8 address_to want = (p8 address_to)aligned_at(base_want_room) + 16 + offset;
        positive got_length = positive_into_base(got, value, base, upper);
        positive want_length = reference_into_base(want, value, base, upper);

        same("positive_into_base", "how many", got_length, want_length);
        same_bytes("positive_into_base", "the digits", got, want, want_length);

        for (positive back = 1; back <= 8; back++)
                same("positive_into_base", "in front of it",
                     (positive)*(got - back), (positive)guard);

        for (positive after = 0; after < 8; after++)
                same("positive_into_base", "behind it",
                     (positive)got[got_length + after], (positive)guard);
}

fn check_into_base()
{
        static positive tree_bases[] = {2, 4, 8, 10, 16, 32};
        static positive invalid_bases[] = {0, 1, 37, ~(positive)0};
        static p8 guards[] = {0x00, '0', '9', 'x', 0xff};
        positive maximum = ~(positive)0;

        for (positive b = 0;
             b < sizeof(invalid_bases) / sizeof(invalid_bases[0]); b++)
                for (bool upper = false; upper <= true; upper++)
                        for (positive value = 0; value <= 1; value++)
                                for (positive offset = 0; offset < 8; offset++)
                                {
                                        reference_fill(base_got_room, 0xa5,
                                                       sizeof(base_got_room));
                                        p8 address_to got =
                                            (p8 address_to)aligned_at(base_got_room) +
                                            16 + offset;
                                        positive length = positive_into_base(
                                            got, value ? maximum : 0,
                                            invalid_bases[b], upper);

                                        same("positive_into_base", "invalid length",
                                             length, 0);

                                        for (positive i = 0;
                                             i < sizeof(base_got_room); i++)
                                                same("positive_into_base",
                                                     "invalid base wrote nothing",
                                                     base_got_room[i], 0xa5);
                                }

        // Complete small-integer domain for every in-tree base and alphabet.
        for (positive b = 0; b < sizeof(tree_bases) / sizeof(tree_bases[0]); b++)
                for (bool upper = false; upper <= true; upper++)
                        for (positive value = 0; value <= 65535; value++)
                                check_into_base_one(
                                    value, tree_bases[b], upper,
                                    (value + tree_bases[b] + upper) & 7,
                                    guards[(value + b + upper) % sizeof(guards)]);

        // Full alignment/guard cross at every length transition and UINT64.
        for (positive b = 0; b < sizeof(tree_bases) / sizeof(tree_bases[0]); b++)
                for (bool upper = false; upper <= true; upper++)
                        for (positive offset = 0; offset < 8; offset++)
                                for (positive g = 0; g < sizeof(guards); g++)
                                {
                                        positive base = tree_bases[b];
                                        positive power = 1;

                                        check_into_base_one(maximum, base, upper,
                                                            offset, guards[g]);
                                        check_into_base_one(maximum - 1, base, upper,
                                                            offset, guards[g]);

                                        while (1)
                                        {
                                                check_into_base_one(power - 1, base, upper,
                                                                    offset, guards[g]);
                                                check_into_base_one(power, base, upper,
                                                                    offset, guards[g]);
                                                check_into_base_one(power + 1, base, upper,
                                                                    offset, guards[g]);

                                                if (power > maximum / base)
                                                        break;

                                                power *= base;
                                        }
                                }

        // The reusable surface, not only its current consumers.
        for (positive base = 2; base <= 36; base++)
                for (bool upper = false; upper <= true; upper++)
                {
                        check_into_base_one(maximum, base, upper,
                                            base & 7, guards[base % sizeof(guards)]);

                        for (positive r = 0; r < 128; r++)
                        {
                                positive value = next();

                                check_into_base_one(
                                    value, base, upper, (value + r) & 7,
                                    guards[(value + base + upper) % sizeof(guards)]);
                        }
                }
}

/*
        The bounded copy that says where it ended.

        The destination is filled with a byte that is not a terminator and the
        bytes past the answer are checked, because the failure this can have
        and still look right is padding: strncpy zeroes out to the bound, and a
        routine that did that here would answer the right end and flatten
        whatever the caller had already written after it.

        The source is built with more bytes after its terminator, at eight
        offsets, so a copy that reads a word past the end takes them.
*/
static p8 copy_room[1024];
static p8 copy_twin[1024];

p8 address_to reference_copy_max_end(p8 address_to into, string_address source,
                                     positive bound)
{
        positive n = 0;

        while (n < bound && source[n])
        {
                into[n] = source[n];
                n++;
        }

        into[n] = end;

        return into + n;
}

fn check_copy_max_end()
{
        static p8 fillers[] = {0x00, 'z', 0xff, '/'};

        for (positive f = 0; f < sizeof(fillers); f++)
                for (positive offset = 0; offset < 8; offset++)
                        for (positive length = 0; length <= 40; length++)
                                for (positive b = 0; b < SPAN_BOUND_COUNT; b++)
                                {
                                        positive bound = span_bounds[b];
                                        p8 address_to source;
                                        p8 address_to got;
                                        p8 address_to want;
                                        p8 address_to got_end;
                                        p8 address_to want_end;

                                        reference_fill(span_room, 'S', sizeof(span_room));
                                        reference_fill(copy_room, fillers[f],
                                                       sizeof(copy_room));
                                        reference_fill(copy_twin, fillers[f],
                                                       sizeof(copy_twin));

                                        source = span_subject(offset);

                                        for (positive i = 0; i < length; i++)
                                                source[i] = (p8)('a' + i % 26);

                                        // The terminator, and then more bytes
                                        // that are not one: a read past it
                                        // takes them and the answer grows.
                                        source[length] = end;

                                        got = (p8 address_to)aligned_at(copy_room) +
                                              SPAN_HEAD + ((offset + 5) & 7);
                                        want = (p8 address_to)aligned_at(copy_twin) +
                                               SPAN_HEAD + ((offset + 5) & 7);

                                        got_end = string_copy_max_end(got, source, bound);
                                        want_end = reference_copy_max_end(want, source,
                                                                          bound);

                                        same("string_copy_max_end", "the end",
                                             (positive)(got_end - got),
                                             (positive)(want_end - want));

                                        // The bytes written, the terminator,
                                        // and eight past it that must not have
                                        // been touched.
                                        same_bytes("string_copy_max_end", "the bytes",
                                                   got, want,
                                                   (positive)(want_end - want) + 9);

                                        for (positive back = 1; back <= 8; back++)
                                                same("string_copy_max_end",
                                                     "in front of it",
                                                     (positive)*(got - back),
                                                     (positive)fillers[f]);
                                }
}


fn check_hostile_neighbours()
{
        // The byte hunted for, and so also the byte written in front of the
        // string and behind its terminator.
        static p8 hunted[] = {0x00, 0x01, 0x02, 0x40, 0x7f, 0x80,
                              0xa5, 0xfe, 0xff, 'a'};

        hunt_byte or_end =
            verify_or_end_public ? verify_or_end_public : verify_or_end_libc;
        hunt_bounded first_max =
            verify_first_max_public ? verify_first_max_public : verify_first_max_libc;
        hunt_byte last_or_end =
            verify_last_or_end_public ? verify_last_or_end_public
                                      : verify_last_or_end_libc;
        hunt_memory memory_first =
            verify_memory_first_public ? verify_memory_first_public
                                       : verify_memory_first_libc;
        measure_bounded length_max =
            verify_length_max_public ? verify_length_max_public
                                     : verify_length_max_libc;
        compare_bounded compare_max =
            verify_compare_max_public ? verify_compare_max_public
                                      : verify_compare_max_libc;

        if (!or_end)
                absent("string_first_of_or_end");
        if (!first_max)
                absent("string_first_of_max");
        if (!last_or_end)
                absent("string_last_of_or_end");
        if (!memory_first)
                absent("memory_first_of");
        if (!length_max)
                absent("string_length_max");
        if (!compare_max)
                absent("string_compare_max");

        for (positive h = 0; h < sizeof(hunted); h++)
        {
                p8 byte = hunted[h];

                for (positive offset = 0; offset < 8; offset++)
                        for (positive size = 0; size <= 24; size++)
                                for (positive planted = 0; planted < 2; planted++)
                                {
                                        string_address text;
                                        string_address twin;

                                        if (planted && !size)
                                                continue;

                                        reference_fill(field, byte, sizeof(field));
                                        reference_fill(field_twin, byte,
                                                       sizeof(field_twin));

                                        text = aligned_at(field) + 64 + offset;

                                        // The other string of a compare starts
                                        // at a different distance into its
                                        // word, so the two are never in step.
                                        twin = aligned_at(field_twin) + 64 +
                                               ((offset + 3) & 7);

                                        // Beginning with the next value up
                                        // from the byte in front of it, which
                                        // is the pair the borrow lies about.
                                        for (positive i = 0; i < size; i++)
                                        {
                                                p8 value = (p8)(byte + 1 + (i % 3));

                                                text[i] = value ? value : 1;
                                        }

                                        text[size] = 0;

                                        // Once with the hunted byte nowhere in
                                        // the string, so the answer has to be
                                        // nothing, and once with it at the far
                                        // end, so the answer has to be found
                                        // past the word the lie is in.
                                        if (planted)
                                                text[size - 1] = byte;

                                        memory_copy_apart(twin, text, size + 1);

                                        same("string_length", "hostile neighbours",
                                             string_length(text),
                                             reference_length(text));

                                        same("string_first_of", "hostile neighbours",
                                             (positive)string_first_of(text, byte),
                                             (positive)reference_first_of(text, byte));

                                        if (size)
                                                same("string_first_of", "hostile and present",
                                                     (positive)string_first_of(text, text[0]),
                                                     (positive)reference_first_of(text, text[0]));

                                        same("string_last_of", "hostile neighbours",
                                             (positive)string_last_of(text, byte),
                                             (positive)reference_string_last_of(text, byte));

                                        if (or_end)
                                                same("string_first_of_or_end", "hostile neighbours",
                                                     (positive)or_end(text, byte),
                                                     (positive)reference_or_end(text, byte));

                                        if (last_or_end)
                                                same("string_last_of_or_end", "hostile neighbours",
                                                     (positive)last_or_end(text, byte),
                                                     (positive)reference_last_or_end(text, byte));

                                        for (positive count = 0; count <= size + 8; count += 3)
                                        {
                                                if (first_max)
                                                        same("string_first_of_max", "hostile neighbours",
                                                             (positive)first_max(text, count, byte),
                                                             (positive)reference_first_max(text, count, byte));

                                                if (memory_first)
                                                        same("memory_first_of", "hostile neighbours",
                                                             (positive)memory_first(text, byte, count),
                                                             (positive)reference_memory_first(text, byte, count));

                                                if (length_max)
                                                        same("string_length_max", "hostile neighbours",
                                                             length_max(text, count),
                                                             reference_length_max(text, count));

                                                if (compare_max)
                                                {
                                                        // Equal, then differing at
                                                        // the last byte inside the
                                                        // bound, which is where a
                                                        // word compare that stops
                                                        // early stops noticing.
                                                        same("string_compare_max", "hostile equal",
                                                             (positive)(compare_max(text, twin, count) == 0),
                                                             (positive)(reference_compare_max(text, twin, count) == 0));

                                                        if (size)
                                                        {
                                                                p8 keep = twin[size - 1];

                                                                twin[size - 1] = (p8)(keep ^ 1);
                                                                same("string_compare_max", "hostile greater",
                                                                     (positive)(compare_max(text, twin, count) > 0),
                                                                     (positive)(reference_compare_max(text, twin, count) > 0));
                                                                same("string_compare_max", "hostile less",
                                                                     (positive)(compare_max(twin, text, count) < 0),
                                                                     (positive)(reference_compare_max(twin, text, count) < 0));
                                                                twin[size - 1] = keep;
                                                        }
                                                }
                                        }
                                }
        }
}


/*
        memory_count, which has a wide body and a narrow one.

        On x86_64 it compares thirty two bytes at once and drains a byte wide
        accumulator every 255 rounds, so the sizes that matter are the ones
        either side of 32 and either side of 8160. Alignment matters too: the
        wide loop starts wherever the caller's pointer does.

        The reference is the loop it replaced, which is the only thing worth
        comparing against -- an independent second guess at the answer would
        just be a second thing to be wrong.
*/
positive reference_count(address_any block, positive size, b8 value)
{
        p8 address_to at = block;
        positive found = 0;

        for (positive i = 0; i < size; i++)
                if (at[i] == value)
                        found++;

        return found;
}

fn check_count()
{
        static p8 room[9000];

        // every alignment the wide loop can begin on, and every length across
        // the point where it starts and the point where it drains
        for (positive off = 0; off < 40; off++)
                for (positive size = 0; size < 200; size++)
                {
                        for (positive i = 0; i < off + size + 8; i++)
                                room[i] = (p8)((i * 7 + off) % 5 ? 'a' : '\n');

                        for (b8 value = 9; value < 12; value++)
                                same("memory_count", "against the byte loop",
                                     memory_count(room + off, size, value),
                                     reference_count(room + off, size, value));
                }

        // past the drain: 255 rounds of 32 bytes is 8160, and a block of
        // nothing but the wanted byte is where a wrapped accumulator shows
        for (positive size = 8100; size < 8300; size += 7)
        {
                for (positive i = 0; i < size; i++)
                        room[i] = '\n';

                same("memory_count", "every byte matches", memory_count(room, size, '\n'), size);
                same("memory_count", "no byte matches", memory_count(room, size, 'z'), 0);
        }
}


/*
        memory_first_of at the sizes that reach the wide path.

        Everything else that hunts a byte in this file works on short strings,
        and on x86_64 memory_first_of takes a thirty two byte at a time route
        that none of them are long enough to enter. A one byte error in it
        passed the whole suite -- 263472 checks -- which is what a test that
        cannot reach the code it is aimed at is worth.

        So: every alignment the wide loop can begin on, every length either
        side of thirty two and of a few multiples of it, and the wanted byte
        placed at each position in turn, including the last one before the
        bound and the first one past it.
*/
address_any reference_memory_first_of(address_any block, b8 value, positive size)
{
        p8 address_to at = block;

        for (positive i = 0; i < size; i++)
                if (at[i] == value)
                        return at + i;

        return null;
}

fn check_first_of_wide()
{
        static p8 room[600];

        for (positive off = 0; off < 34; off++)
                for (positive size = 0; size < 140; size++)
                {
                        // nothing to find: the loop has to walk the whole bound
                        for (positive i = 0; i < off + size + 40; i++)
                                room[i] = 'a';

                        same("memory_first_of", "absent over the wide path",
                             (positive)memory_first_of(room + off, 'z', size),
                             (positive)reference_memory_first_of(room + off, 'z', size));

                        // and one to find, at every position it could sit
                        for (positive where = 0; where < size; where++)
                        {
                                room[off + where] = 'z';

                                same("memory_first_of", "found over the wide path",
                                     (positive)memory_first_of(room + off, 'z', size),
                                     (positive)reference_memory_first_of(room + off, 'z', size));

                                room[off + where] = 'a';
                        }

                        // just past the bound, which must not be reported
                        room[off + size] = 'z';

                        same("memory_first_of", "one past the bound",
                             (positive)memory_first_of(room + off, 'z', size),
                             (positive)reference_memory_first_of(room + off, 'z', size));

                        room[off + size] = 'a';
                }
}


/*
        The bulk routines, tier by tier and alignment by alignment.

        Two things the sweeps above cannot reach.

        The first is the tier. memory_fill, memory_copy_apart and the routines
        that hand work to them have three bodies on x86_64 -- zmm, ymm, and
        eight bytes in an integer register -- and cpu_detect picks one at
        startup. On a machine with AVX-512 that is the only one that ever runs,
        so a one byte error in the other two passes every check in this file.
        cpu_has_avx2 and cpu_has_avx512 are ordinary writable bytes read on
        every call, so the whole sweep runs three times with the byte forced
        down a tier each round and put back afterwards. Nothing forces a tier
        up: a processor cannot be told to have registers it does not have.

        The second is alignment. Everything above works at a fixed offset into
        a static buffer, so the align-up step in each of these -- which moves
        the destination by one of eight values on riscv64, 32 on the ymm path
        and 64 on the zmm and arm64 ones, and moves the source by the same --
        has only ever been entered with one of them. Destination offset crossed
        with source offset is what makes the other sixty three run.
*/
#define BULK_ROOM 1152

static b8 bulk_got[BULK_ROOM];
static b8 bulk_want[BULK_ROOM];
static b8 bulk_from[BULK_ROOM];

// Poisoned either side, so a routine that writes outside its size is caught
// rather than tolerated. The whole buffer is compared, not only the part that
// was meant to change.
fn bulk_same(string_address what, positive tier, positive size, positive off)
{
        checks++;

        for (positive i = 0; i < BULK_ROOM; i++)
                if (bulk_got[i] != bulk_want[i])
                {
                        failures++;
                        string_format(log, "  FAIL %s tier %p size %p off %p: byte %p is %p want %p\n",
                                      what, tier, size, off, i,
                                      (positive)bulk_got[i], (positive)bulk_want[i]);
                        return;
                }
}

// Every size a rung of any of the three ladders starts or ends at, and the
// ones either side of it, up past the widest loop body (256 bytes on zmm,
// which needs 512 before the loop is entered at all).
static positive bulk_sizes[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
        23, 24, 25, 31, 32, 33, 39, 40, 47, 48, 55, 56, 63, 64, 65,
        71, 72, 95, 96, 127, 128, 129, 130, 159, 160, 191, 192,
        255, 256, 257, 258, 287, 288, 319, 320, 383, 384,
        511, 512, 513, 514, 543, 544, 575, 576, 639, 640, 700};

#define BULK_SIZE_COUNT (sizeof(bulk_sizes) / sizeof(bulk_sizes[0]))

static positive bulk_source_offsets[] = {0, 1, 2, 3, 5, 7, 8};

#define BULK_SOURCE_COUNT (sizeof(bulk_source_offsets) / sizeof(bulk_source_offsets[0]))

#if X64
#define BULK_TIERS 3
#else
#define BULK_TIERS 1
#endif

fn bulk_tier(positive pass, p8 wide, p8 widest)
{
#if X64
        cpu_has_avx2 = pass < 2 ? wide : 0;
        cpu_has_avx512 = pass < 1 ? widest : 0;
#else
        (void)pass;
        (void)wide;
        (void)widest;
#endif
}

fn check_bulk_alignments()
{
        p8 wide = 0;
        p8 widest = 0;

#if X64
        wide = cpu_has_avx2;
        widest = cpu_has_avx512;
#endif

        for (positive i = 0; i < BULK_ROOM; i++)
                bulk_from[i] = (b8)(i * 13 + 5);

        for (positive pass = 0; pass < BULK_TIERS; pass++)
        {
                bulk_tier(pass, wide, widest);

                for (positive e = 0; e < BULK_SIZE_COUNT; e++)
                {
                        positive size = bulk_sizes[e];

                        for (positive off = 0; off < 72; off++)
                        {
                                if (off + size + 200 > BULK_ROOM)
                                        continue;

                                reference_fill(bulk_got, 0xA5, BULK_ROOM);
                                reference_fill(bulk_want, 0xA5, BULK_ROOM);
                                memory_fill(bulk_got + off, (b8)(size + 3), size);
                                reference_fill(bulk_want + off, (b8)(size + 3), size);
                                bulk_same("memory_fill", pass, size, off);

                                for (positive s = 0; s < BULK_SOURCE_COUNT; s++)
                                {
                                        positive from = bulk_source_offsets[s];

                                        reference_fill(bulk_got, 0xA5, BULK_ROOM);
                                        reference_fill(bulk_want, 0xA5, BULK_ROOM);
                                        memory_copy_apart(bulk_got + off, bulk_from + from, size);
                                        reference_copy(bulk_want + off, bulk_from + from, size);
                                        bulk_same("memory_copy_apart", pass, size, off);
                                }
                        }
                }
        }

        bulk_tier(0, wide, widest);
}

/*
        memmove, in one buffer, at every distance that puts the overlap in a
        different place relative to the loop body: inside the head, inside the
        chunk kept in registers, and past the whole of it. A memmove that reads
        the last chunk after it has written over it passes a disjoint copy and
        fails exactly here.
*/
static b32 bulk_gaps[] = {-700, -513, -257, -129, -65, -64, -33, -32, -9, -8,
                          -3, -1, 1, 3, 8, 9, 32, 33, 64, 65, 129, 257, 513, 700};

#define BULK_GAP_COUNT (sizeof(bulk_gaps) / sizeof(bulk_gaps[0]))

static positive bulk_move_sizes[] = {0, 1, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                                     63, 64, 65, 71, 127, 128, 129, 255, 256,
                                     257, 511, 512, 513, 575, 640, 700};

#define BULK_MOVE_COUNT (sizeof(bulk_move_sizes) / sizeof(bulk_move_sizes[0]))

fn check_bulk_moves()
{
        p8 wide = 0;
        p8 widest = 0;

#if X64
        wide = cpu_has_avx2;
        widest = cpu_has_avx512;
#endif

        for (positive i = 0; i < BULK_ROOM; i++)
                bulk_from[i] = (b8)(i * 29 + 11);

        for (positive pass = 0; pass < BULK_TIERS; pass++)
        {
                bulk_tier(pass, wide, widest);

                for (positive e = 0; e < BULK_MOVE_COUNT; e++)
                {
                        positive size = bulk_move_sizes[e];

                        for (positive off = 0; off < 72; off++)
                                for (positive g = 0; g < BULK_GAP_COUNT; g++)
                                {
                                        b32 gap = bulk_gaps[g];
                                        positive base = 200 + off;

                                        if ((b32)base + gap < 0)
                                                continue;

                                        if (base + size + 8 > BULK_ROOM)
                                                continue;

                                        if ((positive)((b32)base + gap) + size + 8 > BULK_ROOM)
                                                continue;

                                        reference_copy(bulk_got, bulk_from, BULK_ROOM);
                                        reference_copy(bulk_want, bulk_from, BULK_ROOM);

                                        memory_copy(bulk_got + base + gap, bulk_got + base, size);
                                        reference_copy(bulk_want + base + gap, bulk_want + base, size);

                                        bulk_same("memory_copy", pass, size, off);
                                }
                }
        }

        bulk_tier(0, wide, widest);
}

/*
        The four string routines against the C they replaced, at every
        alignment and either side of every width they switch strategy at.

        string_copy_max gets a bound that runs past the end of the source and
        one that stops short of it, and the buffer is poisoned to the end
        rather than to 64 bytes: a wide store that ignores the bound writes 32
        or 64 bytes past it, which a check that stops at 64 cannot see.
*/
static p8 bulk_subject[BULK_ROOM];
static p8 bulk_spare[BULK_ROOM];

fn check_bulk_wide_strings()
{
        p8 wide = 0;
        p8 widest = 0;

#if X64
        wide = cpu_has_avx2;
        widest = cpu_has_avx512;
#endif

        for (positive pass = 0; pass < BULK_TIERS; pass++)
        {
                bulk_tier(pass, wide, widest);

                for (positive e = 0; e < BULK_SIZE_COUNT; e++)
                {
                        positive size = bulk_sizes[e];

                        for (positive off = 0; off < 72; off++)
                        {
                                if (off + size + 200 > BULK_ROOM)
                                        continue;

                                p8 address_to text = bulk_subject + off;

                                for (positive i = 0; i < size; i++)
                                        text[i] = (p8)(next() % 4 + 'a');

                                text[size] = 0;

                                // string_copy: length, content, and not a byte
                                // written past the terminator it copied.
                                for (positive d = 0; d < 3; d++)
                                {
                                        reference_fill(bulk_spare, 0xA5, BULK_ROOM);
                                        string_copy(bulk_spare + d, (string_address)text);

                                        checks++;

                                        if (reference_length(bulk_spare + d) != size)
                                                report("string_copy", "length",
                                                       reference_length(bulk_spare + d), size);

                                        checks++;

                                        for (positive i = 0; i < size; i++)
                                                if (bulk_spare[d + i] != text[i])
                                                {
                                                        report("string_copy", "content", i, size);
                                                        break;
                                                }

                                        checks++;

                                        for (positive i = d + size + 1; i < BULK_ROOM; i++)
                                                if (bulk_spare[i] != 0xA5)
                                                {
                                                        report("string_copy", "wrote past the terminator",
                                                               i, size);
                                                        break;
                                                }
                                }

                                // string_copy_max at every bound around the
                                // length, not only the first 64.
                                for (positive b = 0; b < 6; b++)
                                {
                                        positive limit = size + 2 > b ? size + 2 - b : 0;

                                        if (limit + 8 > BULK_ROOM)
                                                continue;

                                        reference_fill(bulk_spare, 0xA5, BULK_ROOM);
                                        string_copy_max(bulk_spare, (string_address)text, limit);

                                        checks++;

                                        positive want = size < limit ? size : limit;

                                        for (positive i = 0; i < want; i++)
                                                if (bulk_spare[i] != text[i])
                                                {
                                                        report("string_copy_max", "content", i, limit);
                                                        break;
                                                }

                                        checks++;

                                        // A terminator only where the source
                                        // ended inside the bound, and nothing
                                        // at all past the bound.
                                        positive first = size < limit ? want + 1 : limit;

                                        if (size < limit && bulk_spare[want] != 0)
                                        {
                                                checks++;
                                                report("string_copy_max", "no terminator",
                                                       (positive)bulk_spare[want], limit);
                                        }

                                        for (positive i = first; i < BULK_ROOM; i++)
                                                if (bulk_spare[i] != 0xA5)
                                                {
                                                        report("string_copy_max", "wrote past the limit",
                                                               i, limit);
                                                        break;
                                                }
                                }

                                // string_cut and string_replace_all change the
                                // string, so each gets its own copy of it -- at
                                // the same offset the source is at, because both
                                // walk to a block boundary before they widen and
                                // a string that always starts aligned never
                                // enters that walk. A one byte error in it
                                // passed the whole file until this offset was
                                // anything but zero.
                                for (positive c = 0; c < 2; c++)
                                {
                                        b8 symbol = c ? 'b' : 'q';
                                        p8 address_to mine = bulk_spare + off;

                                        reference_fill(bulk_spare, 0xA5, BULK_ROOM);

                                        for (positive i = 0; i <= size; i++)
                                                mine[i] = text[i];

                                        // where the cut lands, read before the
                                        // cut writes a terminator over it
                                        string_address where = reference_first_of(
                                            (string_address)mine, (p8)symbol);
                                        positive at = where ? (positive)(where - (string_address)mine) : 0;
                                        p8 following = where ? mine[at + 1] : 0;

                                        string_address got = string_cut((string_address)mine, symbol);

                                        same("string_cut", "answer", (positive)got,
                                             where == null || following == 0
                                                 ? 0
                                                 : (positive)((string_address)mine + at + 1));

                                        checks++;

                                        if (where && mine[at] != 0)
                                                report("string_cut", "did not terminate", size, 0);

                                        checks++;

                                        // and nothing outside the string moved
                                        for (positive i = 0; i < BULK_ROOM; i++)
                                        {
                                                if (i >= off && i <= off + size)
                                                        continue;

                                                if (bulk_spare[i] != 0xA5)
                                                {
                                                        report("string_cut", "wrote outside the string",
                                                               i, size);
                                                        break;
                                                }
                                        }

                                        reference_fill(bulk_spare, 0xA5, BULK_ROOM);

                                        for (positive i = 0; i <= size; i++)
                                                mine[i] = text[i];

                                        string_replace_all((string_address)mine, symbol, 'z');

                                        checks++;

                                        for (positive i = 0; i < size; i++)
                                                if (mine[i] != (text[i] == symbol ? 'z' : text[i]))
                                                {
                                                        report("string_replace_all", "byte", i, size);
                                                        break;
                                                }

                                        checks++;

                                        for (positive i = 0; i < BULK_ROOM; i++)
                                        {
                                                if (i >= off && i < off + size)
                                                        continue;

                                                if (bulk_spare[i] != (i == off + size ? 0 : 0xA5))
                                                {
                                                        report("string_replace_all", "wrote outside the string",
                                                               i, size);
                                                        break;
                                                }
                                        }
                                }
                        }
                }
        }

        bulk_tier(0, wide, widest);
}

/*
        memory_compare and memory_search, and the sizes that reach the paths.

        Both have a body per width. memory_compare walks thirty two bytes at a
        time where the processor has the registers for it, eight where it does
        not and one below that; memory_search compares a whole block of
        positions at once above a length and hunts a byte at a time below it.
        A case that stays short never enters the wide half at all, and a one
        byte error in the wide half of memory_first_of once passed the whole
        suite -- 263472 checks -- for exactly that reason. So the lengths here
        run either side of every width, and on x86_64 the whole set runs a
        second time with cpu_has_avx2 forced off, because that is the only way
        the narrow body is reached on a machine that has the wide one.

        The references are the dumbest thing that answers the question: a byte
        loop, and a triple loop that tries every start. Written that way on
        purpose. string_find and the C beside it once had the same bug -- a
        failed candidate resuming past the bytes it had matched, so that "aab"
        was not found in "aaab" -- and the tests agreed with the bug because
        the reference was clever in the same way.
*/
b32 reference_memory_compare(address_any first, address_any second, positive size)
{
        p8 address_to a = first;
        p8 address_to b = second;

        for (positive i = 0; i < size; i++)
                if (a[i] != b[i])
                        return (b32)a[i] - (b32)b[i];

        return 0;
}

address_any reference_memory_search(address_any block, positive size,
                                    address_any needle, positive needle_size)
{
        p8 address_to hay = block;
        p8 address_to want = needle;

        if (needle_size == 0)
                return block;

        if (needle_size > size)
                return null;

        for (positive at = 0; at + needle_size <= size; at++)
        {
                positive i = 0;

                while (i < needle_size && hay[at + i] == want[i])
                        i++;

                if (i == needle_size)
                        return hay + at;
        }

        return null;
}

static p8 compare_room[400];
static p8 compare_twin[400];

fn check_memory_compare()
{
        for (positive off = 0; off < 5; off++)
                for (positive size = 0; size <= 140; size++)
                {
                        for (positive i = 0; i < 400; i++)
                        {
                                compare_room[i] = (p8)('a' + (i % 7));
                                compare_twin[i] = compare_room[i];
                        }

                        same("memory_compare", "equal",
                             (positive)memory_compare(compare_room + off,
                                                      compare_twin + off, size),
                             (positive)reference_memory_compare(compare_room + off,
                                                                compare_twin + off,
                                                                size));

                        //      Every position for the short ones, and the
                        //      edges of every block for the long ones: the
                        //      first byte, the last, and either side of each
                        //      thirty two and eight byte step.
                        for (positive where = 0; where < size; where++)
                        {
                                if (size > 48 && where > 3 && where + 4 < size &&
                                    (where % 8) > 1 && (where % 32) > 1)
                                        continue;

                                //      Two directions, because the sign of
                                //      the answer is the order of the two
                                //      strings and a compare that gets the
                                //      difference right can still get that
                                //      backwards.
                                compare_twin[off + where] =
                                    (p8)(compare_room[off + where] ^ 0x80);

                                same("memory_compare", "differs high",
                                     (positive)memory_compare(compare_room + off,
                                                              compare_twin + off, size),
                                     (positive)reference_memory_compare(
                                         compare_room + off, compare_twin + off, size));

                                compare_twin[off + where] =
                                    (p8)(compare_room[off + where] + 1);

                                same("memory_compare", "differs by one",
                                     (positive)memory_compare(compare_room + off,
                                                              compare_twin + off, size),
                                     (positive)reference_memory_compare(
                                         compare_room + off, compare_twin + off, size));

                                //      And one past the size, which must not
                                //      be looked at.
                                compare_twin[off + where] = compare_room[off + where];
                        }

                        compare_twin[off + size] = 0xff;

                        same("memory_compare", "one past the size",
                             (positive)memory_compare(compare_room + off,
                                                      compare_twin + off, size),
                             (positive)reference_memory_compare(compare_room + off,
                                                                compare_twin + off,
                                                                size));

                        compare_twin[off + size] = compare_room[off + size];
                }
}

#if RISCV64
/*
        The RV64 base ISA does not require naturally misaligned integer loads
        and stores to complete.  QEMU and many Linux systems emulate them, so
        a result-only test can agree while the shipped instruction stream is
        still outside the floor.  src/test/run supplies the disassembly half
        of this check; this is the semantic half.

        Every pair of pointer residues is crossed here.  Equal residues reach
        the aligned word paths after a byte peel, unequal residues can never
        align in lockstep and must remain bytewise.  The ordinary bulk tests
        cover many offsets, but did not cross all 64 pairs for compares or all
        16 overlap distances for memmove.
*/
#define RV_ALIGN_ROOM 512
#define RV_PAGE_ROOM (4096 * 3)

static p8 rv_align_left[RV_ALIGN_ROOM];
static p8 rv_align_right[RV_ALIGN_ROOM];
static p8 rv_align_got[RV_ALIGN_ROOM];
static p8 rv_align_want[RV_ALIGN_ROOM];
static p8 rv_align_from[RV_ALIGN_ROOM];
static p8 rv_page_left[RV_PAGE_ROOM];
static p8 rv_page_right[RV_PAGE_ROOM];
static p8 rv_page_got[RV_PAGE_ROOM];
static p8 rv_page_want[RV_PAGE_ROOM];

positive reference_trailing_positive(string_address input);

static p8 address_to rv_page(p8 address_to room)
{
        positive address = (positive)(address_any)room;

        return (p8 address_to)((address + 4095) & ~(positive)4095);
}

fn check_riscv_alignment_floor()
{
        static const positive bounds[] = {0, 1, 7, 8, 9, 31, 32, 33, 80, 88};

        for (positive left_offset = 0; left_offset < 8; left_offset++)
                for (positive right_offset = 0; right_offset < 8; right_offset++)
                        for (positive size = 0; size <= 80; size++)
                        {
                                p8 address_to left = aligned_at(rv_align_left) + 64 +
                                                     left_offset;
                                p8 address_to right = aligned_at(rv_align_right) + 64 +
                                                      right_offset;

                                for (positive i = 0; i < 96; i++)
                                {
                                        p8 value = (p8)('a' + (i * 5 + size) % 23);

                                        left[i] = value;
                                        right[i] = value;
                                }

                                left[size] = 0;
                                right[size] = 0;

                                same("memory_compare", "every pair of RV64 residues",
                                     (positive)memory_compare(left, right, size),
                                     (positive)reference_memory_compare(left, right,
                                                                        size));
                                same("string_compare", "every pair of RV64 residues",
                                     (positive)string_compare(left, right),
                                     (positive)reference_compare(left, right));

                                for (positive b = 0;
                                     b < sizeof(bounds) / sizeof(bounds[0]); b++)
                                        same("string_compare_max",
                                             "every pair of RV64 residues",
                                             (positive)string_compare_max(left, right,
                                                                          bounds[b]),
                                             (positive)reference_compare_max(left, right,
                                                                             bounds[b]));

                                for (positive where = 0; where < size; where++)
                                {
                                        right[where] = (p8)(left[where] ^ 0x80);

                                        same("memory_compare", "a difference at every residue",
                                             (positive)memory_compare(left, right, size),
                                             (positive)reference_memory_compare(left, right,
                                                                                size));
                                        same("string_compare", "a difference at every residue",
                                             (positive)string_compare(left, right),
                                             (positive)reference_compare(left, right));
                                        same("string_compare_max",
                                             "a bounded difference at every residue",
                                             (positive)string_compare_max(left, right, size),
                                             (positive)reference_compare_max(left, right,
                                                                             size));

                                        right[where] = left[where];
                                }
                        }

        // Every source/destination residue, every short size, and guards on
        // the complete arena.  Sizes through 80 cross every 4/8/16/32-byte
        // transition in the aligned core as well as the byte fallback.
        for (positive destination_offset = 0; destination_offset < 8;
             destination_offset++)
                for (positive source_offset = 0; source_offset < 8; source_offset++)
                        for (positive size = 0; size <= 80; size++)
                        {
                                p8 address_to got = aligned_at(rv_align_got) + 64 +
                                                    destination_offset;
                                p8 address_to want = aligned_at(rv_align_want) + 64 +
                                                     destination_offset;
                                p8 address_to from = aligned_at(rv_align_from) + 64 +
                                                     source_offset;

                                for (positive i = 0; i < RV_ALIGN_ROOM; i++)
                                {
                                        rv_align_got[i] = 0xa5;
                                        rv_align_want[i] = 0xa5;
                                        rv_align_from[i] = (p8)(i * 29 + size);
                                }

                                memory_copy_apart(got, from, size);
                                reference_copy(want, from, size);
                                same_bytes("memory_copy_apart", "every pair of RV64 residues",
                                           rv_align_got, rv_align_want, RV_ALIGN_ROOM);

                                if (source_offset == 0)
                                {
                                        memory_fill(got, (b8)(size + 3), size);
                                        reference_fill(want, (b8)(size + 3), size);
                                        same_bytes("memory_fill", "every RV64 residue",
                                                   rv_align_got, rv_align_want,
                                                   RV_ALIGN_ROOM);
                                }
                        }

        // Backwards and forwards overlap at every distance modulo eight.
        for (positive offset = 0; offset < 8; offset++)
                for (positive distance = 1; distance <= 16; distance++)
                        for (positive size = 0; size <= 96; size++)
                        {
                                positive base = 128 + offset;

                                for (positive i = 0; i < RV_ALIGN_ROOM; i++)
                                        rv_align_got[i] = rv_align_want[i] =
                                            (p8)(i * 13 + size);

                                memory_copy(rv_align_got + base + distance,
                                            rv_align_got + base, size);
                                reference_copy(rv_align_want + base + distance,
                                               rv_align_want + base, size);
                                same_bytes("memory_copy", "every backward RV64 residue",
                                           rv_align_got, rv_align_want, RV_ALIGN_ROOM);

                                for (positive i = 0; i < RV_ALIGN_ROOM; i++)
                                        rv_align_got[i] = rv_align_want[i] =
                                            (p8)(i * 17 + size);

                                memory_copy(rv_align_got + base,
                                            rv_align_got + base + distance, size);
                                reference_copy(rv_align_want + base,
                                               rv_align_want + base + distance, size);
                                same_bytes("memory_copy", "every forward RV64 residue",
                                           rv_align_got, rv_align_want, RV_ALIGN_ROOM);
                        }

        /*
                Put the exclusive end at an actual 4096-byte boundary.  The
                sixteen bytes beyond it are hostile rather than zero, so a
                scan that skips the terminator or a bulk operation that leaks
                across its bound is visible even on a host that maps the next
                page.  The static audit separately proves the final wide
                access itself is naturally aligned.
        */
        {
                p8 address_to left_edge = rv_page(rv_page_left) + 4096;
                p8 address_to right_edge = rv_page(rv_page_right) + 4096;
                p8 address_to got_edge = rv_page(rv_page_got) + 4096;
                p8 address_to want_edge = rv_page(rv_page_want) + 4096;

                for (positive residue = 0; residue < 8; residue++)
                {
                        positive size = 64 + residue;
                        p8 address_to left = left_edge - size;
                        p8 address_to right = right_edge - size;
                        p8 address_to got = got_edge - size;
                        p8 address_to want = want_edge - size;

                        for (positive i = 0; i < size; i++)
                                left[i] = right[i] = (p8)('a' + (i % 17));

                        left[size - 1] = 0;
                        right[size - 1] = 0;

                        for (positive i = 0; i < 16; i++)
                        {
                                left_edge[i] = 0xe1;
                                right_edge[i] = 0xe2;
                                got_edge[i] = 0xe3;
                                want_edge[i] = 0xe3;
                        }

                        same("string_compare", "a terminator at a page edge",
                             (positive)string_compare(left, right),
                             (positive)reference_compare(left, right));
                        same("string_compare_max", "a bound at a page edge",
                             (positive)string_compare_max(left, right, size),
                             (positive)reference_compare_max(left, right, size));
                        same("memory_compare", "a block ending at a page edge",
                             (positive)memory_compare(left, right, size),
                             (positive)reference_memory_compare(left, right, size));
                        same("memory_count", "a block ending at a page edge",
                             memory_count(left, size, 'a'),
                             reference_count(left, size, 'a'));

                        for (positive i = 0; i < size; i++)
                                got[i] = want[i] = 0xa5;

                        memory_copy_apart(got, left, size);
                        reference_copy(want, left, size);
                        same_bytes("memory_copy_apart", "an end at a page edge",
                                   got, want, size + 16);

                        memory_fill(got, (b8)(residue + 1), size);
                        reference_fill(want, (b8)(residue + 1), size);
                        same_bytes("memory_fill", "an end at a page edge",
                                   got, want, size + 16);

                        for (positive i = 0; i + 1 < size; i++)
                                left[i] = (p8)('0' + (i * 7 + residue) % 10);
                        left[size - 1] = 0;

                        same("string_to_positive", "a terminator at a page edge",
                             string_to_positive(left),
                             reference_trailing_positive(left));
                }
        }
}
#endif

static p8 search_room[400];
static p8 search_needle[200];

//      Lengths either side of both block widths -- sixteen on arm64, thirty
//      two on x86_64 -- and of the four byte probe inside the candidate step.
static const positive search_lengths[] = {1,  2,  3,  4,  5,  6,  7,  8,  15, 16,
                                          17, 31, 32, 33, 40, 63, 64, 65, 100};

fn check_memory_search()
{
        static const string_address alphabets[] = {"ab", "abc", "aaab"};

        //      The empty needle, which memmem answers with the front of the
        //      haystack. string_find disagrees on purpose and has its own
        //      cases elsewhere.
        same("memory_search", "an empty needle",
             (positive)memory_search(search_room, 10, search_room, 0),
             (positive)reference_memory_search(search_room, 10, search_room, 0));

        same("memory_search", "an empty needle in an empty haystack",
             (positive)memory_search(search_room, 0, search_room, 0),
             (positive)reference_memory_search(search_room, 0, search_room, 0));

        for (positive a = 0; a < 3; a++)
        {
                string_address letters = alphabets[a];
                positive count = reference_length(letters);

                for (positive i = 0; i < 400; i++)
                        search_room[i] = (p8)letters[i % count];

                for (positive off = 0; off < 3; off++)
                        for (positive n = 0; n < 19; n++)
                        {
                                positive needle_size = search_lengths[n];

                                for (positive size = 0; size <= 200; size += 7)
                                {
                                        //      A needle that is not there at
                                        //      all: the whole bound has to be
                                        //      walked, including the piece
                                        //      past the last whole block.
                                        for (positive i = 0; i < needle_size; i++)
                                                search_needle[i] = 'z';

                                        same("memory_search", "absent",
                                             (positive)memory_search(search_room + off, size,
                                                                     search_needle, needle_size),
                                             (positive)reference_memory_search(
                                                 search_room + off, size, search_needle,
                                                 needle_size));

                                        //      A needle longer than what is
                                        //      left, which must answer
                                        //      nothing rather than read past
                                        //      the end.
                                        if (needle_size > size)
                                                continue;

                                        //      And one that is there, at
                                        //      every position it could sit --
                                        //      the very first, the very last,
                                        //      and every one between.
                                        for (positive at = 0; at + needle_size <= size; at++)
                                        {
                                                for (positive i = 0; i < needle_size; i++)
                                                        search_needle[i] =
                                                            search_room[off + at + i];

                                                same("memory_search", "present",
                                                     (positive)memory_search(
                                                         search_room + off, size,
                                                         search_needle, needle_size),
                                                     (positive)reference_memory_search(
                                                         search_room + off, size,
                                                         search_needle, needle_size));
                                        }
                                }
                        }
        }

        //      A needle whose two ends are the same byte, which is the pair
        //      the wide path hunts when it has nothing rarer to pick, and a
        //      needle that overlaps itself.
        {
                static const string_address pairs[][2] = {
                    {"aaab", "aab"},
                    {"aaaab", "aab"},
                    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"},
                    {"xaxaxaxb", "axb"},
                    {"abababababc", "ababc"},
                    {"aaaaa", "aaa"},
                    {"aa", "aaa"},
                    {"", "a"},
                    {"a", "a"},
                    {"ababa", "aba"},
                    {"the quick brown fox jumps over the lazy dog", "the lazy"},
                    {"the quick brown fox jumps over the lazy dog", "tot"},
                    {"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzq", "zzq"},
                };

                for (positive i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
                {
                        string_address hay = pairs[i][0];
                        string_address needle = pairs[i][1];
                        positive hay_size = reference_length(hay);
                        positive needle_size = reference_length(needle);

                        same("memory_search", "a match inside a failed candidate",
                             (positive)memory_search((address_any)hay, hay_size,
                                                     (address_any)needle, needle_size),
                             (positive)reference_memory_search((address_any)hay, hay_size,
                                                               (address_any)needle,
                                                               needle_size));
                }
        }

        //      Every byte value as the needle's first and last, so that no
        //      answer depends on the broadcast of a particular byte -- 0xff
        //      and 0x00 among them, which are what a mask built the wrong way
        //      round gets wrong.
        for (positive value = 0; value < 256; value += 5)
        {
                for (positive i = 0; i < 300; i++)
                        search_room[i] = (p8)value;

                for (positive n = 0; n < 12; n++)
                {
                        positive needle_size = search_lengths[n];

                        for (positive i = 0; i < needle_size; i++)
                                search_needle[i] = (p8)value;

                        search_room[200] = (p8)(value ^ 1);

                        same("memory_search", "one byte over and over",
                             (positive)memory_search(search_room, 260, search_needle,
                                                     needle_size),
                             (positive)reference_memory_search(search_room, 260,
                                                               search_needle, needle_size));

                        search_room[200] = (p8)value;
                }
        }
}

static p8 reference_ascii_fold(p8 value)
{
        return value >= 'A' && value <= 'Z' ? (p8)(value + 32) : value;
}

static address_any reference_memory_first_ascii_case(address_any block, b8 value,
                                                       positive size)
{
        p8 address_to bytes = block;
        p8 wanted = reference_ascii_fold((p8)value);

        for (positive i = 0; i < size; i++)
                if (reference_ascii_fold(bytes[i]) == wanted)
                        return bytes + i;

        return null;
}

static address_any reference_memory_search_ascii_case(address_any block, positive size,
                                                        address_any needle,
                                                        positive needle_size)
{
        p8 address_to hay = block;
        p8 address_to want = needle;

        if (!needle_size)
                return block;

        if (needle_size > size)
                return null;

        for (positive at = 0; at + needle_size <= size; at++)
        {
                positive i = 0;

                while (i < needle_size &&
                       reference_ascii_fold(hay[at + i]) ==
                           reference_ascii_fold(want[i]))
                        i++;

                if (i == needle_size)
                        return hay + at;
        }

        return null;
}

/*
        The folded byte hunt and substring search.

        Every target byte crosses every short alignment and bound.  The long
        cases cross both vector widths and put plausible matches at every
        candidate position; high bytes beside ASCII letters prove that OR 32
        is only used to recognize a letter and never to fold punctuation or
        the upper half of the byte range.
*/
fn check_memory_search_ascii_case()
{
        static p8 room[320];
        static p8 needle[80];

        same("memory_first_of_ascii_case", "zero bound null block",
             (positive)memory_first_of_ascii_case(null, 'a', 0), 0);
        same("memory_search_ascii_case", "empty needle",
             (positive)memory_search_ascii_case(room, 20, null, 0),
             (positive)room);
        same("memory_search_ascii_case", "empty both",
             (positive)memory_search_ascii_case(room, 0, null, 0),
             (positive)room);

        for (positive target = 0; target < 256; target++)
                for (positive offset = 0; offset < 9; offset++)
                {
                        for (positive i = 0; i < sizeof(room); i++)
                                room[i] = (p8)(i * 37 + offset * 11);

                        for (positive size = 0; size <= 96; size += 3)
                                same("memory_first_of_ascii_case",
                                     "every byte, alignment and bound",
                                     (positive)memory_first_of_ascii_case(
                                         room + offset, (b8)target, size),
                                     (positive)reference_memory_first_ascii_case(
                                         room + offset, (b8)target, size));
                }

        for (positive offset = 0; offset < 7; offset++)
                for (positive size = 0; size <= 160; size += 5)
                {
                        for (positive i = 0; i < sizeof(room); i++)
                        {
                                static const p8 alphabet[] = {
                                    'a', 'B', 'c', 'D', 'e', ' ', '[', '`',
                                    '{', 0, 0x7f, 0x80, 0xa0, 0xff,
                                };

                                room[i] = alphabet[(i * 5 + offset) % sizeof(alphabet)];
                        }

                        for (positive needle_size = 0; needle_size <= 40;
                             needle_size += 3)
                        {
                                for (positive i = 0; i < needle_size; i++)
                                        needle[i] = (p8)(0xe0 + (i % 13));

                                same("memory_search_ascii_case", "absent and bounded",
                                     (positive)memory_search_ascii_case(
                                         room + offset, size, needle, needle_size),
                                     (positive)reference_memory_search_ascii_case(
                                         room + offset, size, needle, needle_size));

                                if (needle_size > size)
                                        continue;

                                for (positive at = 0; at + needle_size <= size;
                                     at += 7)
                                {
                                        for (positive i = 0; i < needle_size; i++)
                                        {
                                                p8 value = room[offset + at + i];

                                                if (value >= 'a' && value <= 'z' && (i & 1))
                                                        value -= 32;
                                                else if (value >= 'A' && value <= 'Z' &&
                                                         !(i & 1))
                                                        value += 32;

                                                needle[i] = value;
                                        }

                                        same("memory_search_ascii_case",
                                             "present at every candidate region",
                                             (positive)memory_search_ascii_case(
                                                 room + offset, size, needle,
                                                 needle_size),
                                             (positive)reference_memory_search_ascii_case(
                                                 room + offset, size, needle,
                                                 needle_size));
                                }
                        }
                }

        {
                static const string_address pairs[][2] = {
                    {"aaaB", "AaB"},
                    {"abABaba", "aBaBa"},
                    {"the SAME thing twice", "Same Thing"},
                    {"[`{", "[`{"},
                };

                for (positive i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
                {
                        positive hay_size = reference_length(pairs[i][0]);
                        positive needle_size = reference_length(pairs[i][1]);

                        same("memory_search_ascii_case", "overlap and punctuation",
                             (positive)memory_search_ascii_case(
                                 (address_any)pairs[i][0], hay_size,
                                 (address_any)pairs[i][1], needle_size),
                             (positive)reference_memory_search_ascii_case(
                                 (address_any)pairs[i][0], hay_size,
                                 (address_any)pairs[i][1], needle_size));
                }
        }
}


/*
        string_find over a string longer than one chunk.

        string_find asks memory_search about sixty four kilobytes at a time
        and finds the terminator a page at a time, so every case in this file
        that fits in a line reaches neither loop a second time. A needle lying
        across a chunk seam, or a terminator on the last byte of a page, is
        the only thing that says the overlap and the page walk are right.

        Where those two places are is worked out from the address the linker
        gave the array rather than assumed, and that is the whole reason this
        was rewritten: with the positions written down as constants, both
        mutations -- a page read one byte short, and a chunk advanced one byte
        too far -- passed. The seam is wherever the page walk first reaches
        sixty four kilobytes, which depends on how far into a page the array
        begins, and hard coded 65536 is that place only by luck.

        The answer is known rather than computed: the background is seven
        letters repeating and every needle carries a 'z', which the background
        does not, so the only place it can be found is where it was put. A
        reference walk over two hundred thousand bytes would say the same
        thing and take an emulator a long time to say it.
*/
static p8 long_room[200000];

#define LONG_ROOM_SIZE (sizeof(long_room) - 1)

fn long_room_fill()
{
        for (positive i = 0; i < sizeof(long_room); i++)
                long_room[i] = (p8)('a' + (i % 7));

        long_room[LONG_ROOM_SIZE] = 0;
}

fn check_find_long()
{
        static positive places[64];
        static const positive sizes[] = {2, 3, 4, 5, 8, 33, 64};
        static p8 needle[80];

        positive base = (positive)(address_any)long_room;
        positive page = (4096 - (base & 4095)) & 4095;
        positive seam;
        positive count = 0;

        //      The first page boundary inside the array, and the seam where
        //      string_find stops taking pages and searches what it has.
        if (page == 0)
                page = 4096;

        seam = page;

        while (seam < 65536)
                seam += 4096;

        places[count++] = 0;
        places[count++] = 1;
        places[count++] = 100;

        for (positive i = 0; i < 5; i++)
        {
                places[count++] = page - 4 + i;
                places[count++] = page + 4092 + i;
                places[count++] = seam - 68 + i;
                places[count++] = seam - 4 + i;
                places[count++] = seam + 4092 + i;
        }

        places[count++] = 2 * seam - 3;
        places[count++] = 2 * seam - 1;
        places[count++] = 190000;
        places[count++] = LONG_ROOM_SIZE - 70;

        long_room_fill();

        for (positive p = 0; p < count; p++)
                for (positive n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++)
                {
                        positive at = places[p];
                        positive size = sizes[n];

                        if (at + size + 2 >= LONG_ROOM_SIZE)
                                continue;

                        //      A needle the background cannot hold: 'z' is
                        //      not one of the seven letters.
                        for (positive i = 0; i < size; i++)
                                needle[i] = (p8)(i == 0 || i + 1 == size
                                                     ? 'z'
                                                     : 'a' + (i % 7));

                        needle[size] = 0;

                        for (positive i = 0; i < size; i++)
                                long_room[at + i] = needle[i];

                        same("string_find", "over a string of chunks",
                             (positive)string_find((string_address)long_room,
                                                   (string_address)needle),
                             (positive)(long_room + at));

                        //      And with the string ending just after it, so
                        //      the last chunk is a short one.
                        long_room[at + size + 1] = 0;

                        same("string_find", "in the last short chunk",
                             (positive)string_find((string_address)long_room,
                                                   (string_address)needle),
                             (positive)(long_room + at));

                        long_room[at + size + 1] = (p8)('a' + ((at + size + 1) % 7));

                        //      The terminator immediately before it, so the
                        //      needle is past the end of the string and must
                        //      not be found. Where at is one short of a page
                        //      boundary this is the case that says the page
                        //      walk read the whole page.
                        long_room[at] = 0;

                        same("string_find", "past the terminator",
                             (positive)string_find((string_address)long_room,
                                                   (string_address)needle),
                             0);

                        for (positive i = 0; i < size + 1; i++)
                                long_room[at + i] = (p8)('a' + ((at + i) % 7));
                }

        //      A terminator on the last byte a page holds, with the needle
        //      just past it. A page walk that reads one byte less than a page
        //      steps over this terminator, carries on into the next page and
        //      answers with a match that is not in the string.
        for (positive p = 0; p < 3; p++)
                for (positive step = 0; step < 3; step++)
                {
                        positive edge = page + p * 4096 - 1 + step;
                        static const string_address needle_past = "zzz";

                        long_room_fill();

                        long_room[edge] = 0;
                        long_room[edge + 1] = 'z';
                        long_room[edge + 2] = 'z';
                        long_room[edge + 3] = 'z';

                        same("string_find", "a terminator at the edge of a page",
                             (positive)string_find((string_address)long_room,
                                                   needle_past),
                             0);

                        //      And the same needle inside the string, one
                        //      byte the other side of the same boundary.
                        long_room[edge - 3] = 'z';
                        long_room[edge - 2] = 'z';
                        long_room[edge - 1] = 'z';

                        same("string_find", "a match at the edge of a page",
                             (positive)string_find((string_address)long_room,
                                                   needle_past),
                             (positive)(long_room + edge - 3));
                }

        //      Nothing planted at all, so the whole two hundred thousand
        //      bytes are walked and the answer is nothing.
        {
                static const string_address absent = "zzz";

                long_room_fill();

                same("string_find", "absent over a string of chunks",
                     (positive)string_find((string_address)long_room, absent), 0);
        }
}


/*
        string_compare and string_compare_max over the lengths that reach the
        wide path.

        Both take a vector body once the two have agreed for long enough --
        thirty two bytes on x86_64, thirty two on arm64 -- and every case
        above them in this file is a word or a name, which never gets there.
        So: strings hundreds of bytes long, the difference walked over every
        position around each block edge, and the terminator moved through the
        same places, because the mask those bodies build answers "differs or
        ends" in one and either half of it can be wrong on its own.
*/
static p8 wide_left[700];
static p8 wide_right[700];

//      Under its own name rather than through a prototype, the way the
//      bounded routines above are reached: string_compare_max is assembly and
//      the library declares no C name for it.
WEAK b32 wide_compare_max(string_address a, string_address b, positive count)
    __asm__("string_compare_max");

fn check_compare_wide()
{
        static const positive lengths[] = {0,  1,  7,  8,  15,  16,  17,  31,
                                           32, 33, 40, 63, 64,  65,  96,  127,
                                           128, 129, 160, 200, 300};
        static const positive bounds[] = {0,  1,  8,  31, 32,  33,  63,  64,
                                          65, 96, 128, 129, 200, 400};

        for (positive off = 0; off < 5; off++)
                for (positive l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++)
                {
                        positive length = lengths[l];
                        string_address left = (string_address)(wide_left + off);
                        string_address right = (string_address)(wide_right + off);

                        for (positive i = 0; i < 700; i++)
                        {
                                wide_left[i] = (p8)('a' + (i % 13));
                                wide_right[i] = wide_left[i];
                        }

                        wide_left[off + length] = 0;
                        wide_right[off + length] = 0;

                        //      Past the terminator the two disagree, which is
                        //      what says a bounded compare stopped there.
                        //      With the tails equal, a body that ignores the
                        //      terminator altogether answers the same thing
                        //      and nothing here notices -- that mutation
                        //      survived until this loop was added.
                        for (positive i = off + length + 1; i < 700; i++)
                                wide_right[i] = (p8)(wide_left[i] ^ 0x55);

                        same("string_compare", "long and equal",
                             (positive)string_compare(left, right),
                             (positive)reference_compare(left, right));

                        for (positive b = 0; b < sizeof(bounds) / sizeof(bounds[0]); b++)
                                same("string_compare_max", "long and equal",
                                     (positive)wide_compare_max(left, right, bounds[b]),
                                     (positive)reference_compare_max(left, right, bounds[b]));

                        for (positive where = 0; where < length; where++)
                        {
                                //      Every position while they are short,
                                //      and the edges of every block once they
                                //      are not.
                                if (length > 40 && where > 3 &&
                                    (where % 16) > 1 && (where % 32) > 1 &&
                                    (where % 8) > 1 && where + 3 < length)
                                        continue;

                                wide_right[off + where] =
                                    (p8)(wide_left[off + where] + 1);

                                same("string_compare", "a byte higher",
                                     (positive)string_compare(left, right),
                                     (positive)reference_compare(left, right));
                                same("string_compare", "a byte lower",
                                     (positive)string_compare(right, left),
                                     (positive)reference_compare(right, left));

                                for (positive b = 0; b < sizeof(bounds) / sizeof(bounds[0]); b++)
                                {
                                        same("string_compare_max", "a byte higher",
                                             (positive)wide_compare_max(left, right, bounds[b]),
                                             (positive)reference_compare_max(left, right, bounds[b]));
                                        same("string_compare_max", "a byte lower",
                                             (positive)wide_compare_max(right, left, bounds[b]),
                                             (positive)reference_compare_max(right, left, bounds[b]));
                                }

                                //      And the same position as a terminator
                                //      rather than a difference, which is the
                                //      other half of the mask.
                                wide_right[off + where] = 0;

                                same("string_compare", "an early terminator",
                                     (positive)string_compare(left, right),
                                     (positive)reference_compare(left, right));
                                same("string_compare", "an early terminator, the other way",
                                     (positive)string_compare(right, left),
                                     (positive)reference_compare(right, left));

                                wide_right[off + where] = wide_left[off + where];
                        }
                }
}


/*
        The byte hunts at the lengths a vector body reaches.

        check_hostile_neighbours above stops at twenty four bytes and
        check_first_of_wide covers memory_first_of alone, so every other hunt
        in this file is checked only on strings shorter than one vector
        register. A thirty two or sixty four byte body could be wrong in every
        lane and the suite would still agree with itself -- which is how a one
        byte error in memory_first_of once passed 263472 checks.

        So: every alignment a sixty four byte loop can begin on, lengths
        across 32, 64, 128, 256 and 320, and the hunted byte at every position
        including the last one inside a bound and the first one past it.

        The field is filled with one letter and the hunted byte planted into
        it, so the answer is decided by the plant and not by the noise around
        it: a wide body that finds the right byte in the wrong lane is off by
        a known amount rather than off by luck.
*/

static p8 wide_field[1024];

// Sixty four byte aligned, so an offset from here is the whole of the
// distance into a vector however the linker placed the array.
static string_address wide_aligned_at(p8 address_to array)
{
        positive at = (positive)(address_any)array;

        return array + ((64 - (at & 63)) & 63);
}

static hunt_byte wide_or_end;
static hunt_bounded wide_first_max;
static hunt_byte wide_last_or_end;
static hunt_memory wide_memory_first;
static measure_bounded wide_length_max;

fn wide_bind()
{
        wide_or_end =
            verify_or_end_public ? verify_or_end_public : verify_or_end_libc;
        wide_first_max = verify_first_max_public ? verify_first_max_public
                                                 : verify_first_max_libc;
        wide_last_or_end = verify_last_or_end_public ? verify_last_or_end_public
                                                     : verify_last_or_end_libc;
        wide_memory_first = verify_memory_first_public
                                ? verify_memory_first_public
                                : verify_memory_first_libc;
        wide_length_max = verify_length_max_public ? verify_length_max_public
                                                   : verify_length_max_libc;
}

// The five unbounded hunts and the three bounded ones, the bounded ones at
// the counts a wide loop stops on: nothing at all, half way, exactly the
// string, one past its terminator, and far enough past to need another turn.
fn wide_case(string_address text, positive size, p8 byte)
{
        positive bounds[6];

        same("string_length", "wide", string_length(text), reference_length(text));

        same("string_first_of", "wide", (positive)string_first_of(text, byte),
             (positive)reference_first_of(text, byte));

        same("string_last_of", "wide", (positive)string_last_of(text, byte),
             (positive)reference_string_last_of(text, byte));

        if (wide_or_end)
                same("string_first_of_or_end", "wide",
                     (positive)wide_or_end(text, byte),
                     (positive)reference_or_end(text, byte));

        if (wide_last_or_end)
                same("string_last_of_or_end", "wide",
                     (positive)wide_last_or_end(text, byte),
                     (positive)reference_last_or_end(text, byte));

        bounds[0] = 0;
        bounds[1] = 1;
        bounds[2] = size / 2;
        bounds[3] = size;
        bounds[4] = size + 1;
        bounds[5] = size + 40;

        for (positive i = 0; i < 6; i++)
        {
                positive count = bounds[i];

                if (wide_first_max)
                        same("string_first_of_max", "wide",
                             (positive)wide_first_max(text, count, byte),
                             (positive)reference_first_max(text, count, byte));

                if (wide_memory_first)
                        same("memory_first_of", "wide",
                             (positive)wide_memory_first(text, byte, count),
                             (positive)reference_memory_first(text, byte, count));

                if (wide_length_max)
                        same("string_length_max", "wide",
                             wide_length_max(text, count),
                             reference_length_max(text, count));
        }
}

// Lengths either side of every width a body here could step by, and enough
// of the short end to catch a wide path entered when it should not be.
static positive wide_sizes[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
    14,  15,  16,  17,  18,  19,  20,  23,  24,  25,  30,  31,  32,  33,
    34,  35,  39,  40,  47,  48,  55,  56,  62,  63,  64,  65,  66,  67,
    70,  79,  80,  95,  96,  97,  111, 112, 120, 126, 127, 128, 129, 130,
    131, 143, 144, 159, 160, 175, 176, 191, 192, 193, 200, 223, 224, 240,
    252, 254, 255, 256, 257, 258, 260, 280, 288, 300, 310, 318, 319, 320};

#define WIDE_SIZE_COUNT (sizeof(wide_sizes) / sizeof(wide_sizes[0]))

// The alignments a position sweep runs at. Every one of them is a distance
// into a sixty four byte block that a wide loop has to mask off; the full
// nought to seventy one sweep below covers the rest with fewer plants.
static positive wide_offsets[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63};

#define WIDE_OFFSET_COUNT (sizeof(wide_offsets) / sizeof(wide_offsets[0]))

fn hunts_wide_sweep()
{
        string_address base = wide_aligned_at(wide_field);

        //
        //      Every alignment a sixty four byte loop can begin on, against
        //      every length in the ladder, with the byte in the places a
        //      body gets wrong when its edges are off by one: nowhere at
        //      all, at the very front, at the last byte of the string, and
        //      one past the terminator where nothing unbounded may see it.
        //
        for (positive offset = 0; offset < 72; offset++)
                for (positive s = 0; s < WIDE_SIZE_COUNT; s++)
                {
                        positive size = wide_sizes[s];
                        string_address text = base + offset;

                        if (offset + size + 48 >= sizeof(wide_field))
                                continue;

                        reference_fill(wide_field, 'a', sizeof(wide_field));
                        text[size] = 0;

                        // absent, and present in every byte
                        wide_case(text, size, 'z');
                        wide_case(text, size, 'a');

                        // the terminator itself, which string_first_of and
                        // string_last_of_or_end answer with and string_last_of
                        // answers nothing for
                        wide_case(text, size, 0);

                        if (size)
                        {
                                text[size - 1] = 'z';
                                wide_case(text, size, 'z');
                                text[size - 1] = 'a';
                        }

                        // past the terminator: bounded hunts wide enough may
                        // see it, unbounded ones never may
                        text[size + 1] = 'z';
                        wide_case(text, size, 'z');
                        text[size + 1] = 'a';

                        // a high byte, in case a lane is compared signed
                        text[size / 2] = 0xff;
                        wide_case(text, size, 0xff);
                        text[size / 2] = 'a';
                }

        //
        //      The byte at every position in turn, which is the case a body
        //      that finds the right vector and the wrong lane fails and
        //      nothing else does.
        //
        for (positive o = 0; o < WIDE_OFFSET_COUNT; o++)
                for (positive s = 0; s < WIDE_SIZE_COUNT; s++)
                {
                        positive offset = wide_offsets[o];
                        positive size = wide_sizes[s];
                        string_address text = base + offset;

                        if (offset + size + 48 >= sizeof(wide_field))
                                continue;

                        reference_fill(wide_field, 'a', sizeof(wide_field));
                        text[size] = 0;

                        for (positive where = 0; where <= size + 2; where++)
                        {
                                // that byte is the terminator; moving it
                                // would be a different string
                                if (where == size)
                                        continue;

                                text[where] = 'z';
                                wide_case(text, size, 'z');
                                text[where] = 'a';
                        }
                }
}

/*
        The same sweep once for each body the routines have.

        On x86_64 every hunt below carries three: sixty four bytes at a time
        where the processor has AVX-512, thirty two where it has AVX2, and
        eight where it has neither. cpu_detect answers once at startup, so on
        this machine only the first of the three would ever run and the other
        two would be tested by nothing at all -- which is the same hole a one
        byte error in memory_first_of sat in while 263472 checks agreed.

        The two bytes it wrote are bytes, so the sweep is run again with them
        forced down. Off every architecture that has no such choice this is
        three identical passes, which costs time and proves the same thing
        three times rather than proving nothing once.
*/
fn check_hunts_wide()
{
        p8 avx2 = cpu_has_avx2;
        p8 avx512 = cpu_has_avx512;

        wide_bind();

        hunts_wide_sweep();

        cpu_has_avx512 = 0;
        hunts_wide_sweep();

        cpu_has_avx2 = 0;
        hunts_wide_sweep();

        cpu_has_avx2 = avx2;
        cpu_has_avx512 = avx512;
}

/*
        The six number routines, deeply.

        check_bulk_numbers above is thirteen values and a round trip. That is
        enough to catch a routine that does not work at all and nothing else:
        every one of its samples is under 2^32, so a conversion that splits
        the number into eight digit chunks would never have its second chunk
        reached, and a parser that reads eight bytes at a time would never
        read a second word. Every path here needs a case that lands on it.

        So: every value from zero to a hundred thousand, then every power of
        ten and either side of it, the top and bottom of both types, and for
        the parsers the malformed inputs -- nothing, a sign on its own,
        blanks, a digit string longer than the type holds.
*/

// The C that positive_to_string replaced: a digit at a time into a buffer
// backwards, then one call to the writer with the whole run.
fn reference_positive_to_string(writer write, positive number)
{
        b8 buffer[24];
        positive at = sizeof(buffer);

        if (!number)
                buffer[--at] = '0';

        while (number)
        {
                buffer[--at] = (b8)('0' + number % 10);
                number /= 10;
        }

        write(buffer + at, sizeof(buffer) - at);
}

// And what bipolar_to_string did: the sign is its own call, the magnitude is
// the routine above, and the most negative value negates to itself.
fn reference_bipolar_to_string(writer write, bipolar number)
{
        if (number < 0)
        {
                write((address_any) "-", 1);
                reference_positive_to_string(write, (positive)(-(positive)number));
                return;
        }

        reference_positive_to_string(write, (positive)number);
}

/*
        The trailing digits, which is what string_to_positive answers.

        reference_to_positive at the top of this file reads forwards, and the
        two agree only on a string that is all digits: "12a34" is twelve
        forwards and thirty four backwards, and the assembly's comment says
        the backwards answer is the one callers depend on. Checking malformed
        input against the forward reference would report every case as a
        disagreement for a reason that is not the one being looked for, so the
        backwards rule is written out here as its own reference.
*/
positive reference_trailing_positive(string_address input)
{
        positive length = 0;

        while (input[length])
                length++;

        positive value = 0;
        positive place = 1;

        while (length--)
        {
                p8 c = input[length];

                if (c < '0' || c > '9')
                        break;

                value += place * (positive)(c - '0');
                place *= 10;
        }

        return value;
}

// A leading minus and nothing else: a plus is not a sign here, which is what
// the assembly does and what reference_to_bipolar above does not.
bipolar reference_trailing_bipolar(string_address input)
{
        if (string_is(input, '-'))
                return -(bipolar)reference_trailing_positive(input + 1);

        return (bipolar)reference_trailing_positive(input);
}

static b8 spelled[64];
static b8 spelled_twin[64];

// One value through both, compared as bytes rather than as a number, so a
// leading zero or a lost digit is a failure and not a coincidence.
fn one_positive(positive n, string_address detail)
{
        catch_reset();
        positive_to_string(catch_writer, n);

        positive length = caught_length;

        for (positive i = 0; i <= length; i++)
                spelled[i] = caught[i];

        catch_reset();
        reference_positive_to_string(catch_writer, n);

        same("positive_to_string", detail, length, caught_length);
        same_bytes("positive_to_string", detail, spelled, caught, caught_length + 1);

        // and back again, through the parser, which is the other half
        same("string_to_positive", detail, string_to_positive(spelled), n);
}

fn one_bipolar(bipolar n, string_address detail)
{
        catch_reset();
        bipolar_to_string(catch_writer, n);

        positive length = caught_length;

        for (positive i = 0; i <= length; i++)
                spelled_twin[i] = caught[i];

        catch_reset();
        reference_bipolar_to_string(catch_writer, n);

        same("bipolar_to_string", detail, length, caught_length);
        same_bytes("bipolar_to_string", detail, spelled_twin, caught, caught_length + 1);

        same("string_to_bipolar", detail, (positive)string_to_bipolar(spelled_twin),
             (positive)n);
}

fn check_numbers_exhaustive()
{
        // Every value a five digit number can be, which covers every carry
        // the pair table can make and every digit count up to five.
        for (positive n = 0; n <= 100000; n++)
        {
                one_positive(n, (string_address) "0..100000");
                one_bipolar((bipolar)n, (string_address) "0..100000");
                one_bipolar(-(bipolar)n, (string_address) "-100000..0");
        }
}

fn check_numbers_edges()
{
        // Every power of ten and either side of it: where a conversion picks
        // its number of digits wrong it is here, and nowhere else.
        positive power = 1;

        for (positive e = 0; e < 20; e++)
        {
                one_positive(power - 1, (string_address) "under a power of ten");
                one_positive(power, (string_address) "a power of ten");
                one_positive(power + 1, (string_address) "over a power of ten");

                if (power <= 9223372036854775807ull / 10)
                {
                        one_bipolar((bipolar)power - 1, (string_address) "under a power");
                        one_bipolar((bipolar)power, (string_address) "a power");
                        one_bipolar(-(bipolar)power, (string_address) "a negative power");
                }

                power *= 10;
        }

        // The ends of both types. The top of positive is twenty digits, which
        // is the only length a three chunk conversion reaches its last chunk
        // on, and the bottom of bipolar is the value that negates to itself.
        one_positive(0, (string_address) "zero");
        one_positive(18446744073709551615ull, (string_address) "positive max");
        one_positive(18446744073709551614ull, (string_address) "positive max less one");
        one_positive(10000000000000000000ull, (string_address) "twenty digits round");
        one_positive(9999999999999999999ull, (string_address) "nineteen nines");
        one_positive(4294967295u, (string_address) "thirty two bits");
        one_positive(4294967296ull, (string_address) "one past thirty two bits");
        one_positive(99999999u, (string_address) "eight nines");
        one_positive(100000000u, (string_address) "nine digits");
        one_positive(9999999999999999ull, (string_address) "sixteen nines");
        one_positive(10000000000000000ull, (string_address) "seventeen digits");

        one_bipolar(9223372036854775807ll, (string_address) "bipolar max");
        one_bipolar(-9223372036854775807ll - 1, (string_address) "bipolar min");
        one_bipolar(0, (string_address) "signed zero");
        one_bipolar(-1, (string_address) "minus one");

        // A spread nobody chose by hand, over every digit count there is.
        for (positive i = 0; i < 4000; i++)
        {
                positive digits = 1 + i % 20;
                positive top = 1;

                for (positive e = 1; e < digits; e++)
                        top *= 10;

                positive n = top + next() % (top * 9 + 1);

                one_positive(n, (string_address) "a spread of digit counts");
                one_bipolar((bipolar)(n >> 1), (string_address) "a spread, positive");
                one_bipolar(-(bipolar)(n >> 1), (string_address) "a spread, negative");
        }
}

/*
        What the parsers do with what nobody meant to hand them.

        Both read backwards from the terminator, so the interesting inputs are
        not the ones with a bad byte at the front -- they are the ones where
        the run of digits ends part way, where there are no digits at all, and
        where there are more digits than the type holds and the value has to
        wrap the same way the reference does.
*/
static string_address malformed[] = {
    "", "-", "+", "  ", "abc", "-abc", "+abc", ".", "-.", " - ",
    "0", "-0", "+0", "00000", "-00000", "007", "-007",
    "12a34", "-12a34", "1-2", "a1", "-a1", "1a", "-1a", " 12", "12 ",
    "1 2", "\t9", "9\t", "0x1f", "1e9", "--5", "++5", "-+5", "+-5",
    "18446744073709551615", "18446744073709551616", "18446744073709551617",
    "-18446744073709551615", "99999999999999999999",
    "999999999999999999999999999999", "9223372036854775807",
    "-9223372036854775808", "9223372036854775808", "-9223372036854775809",
    "4294967295", "4294967296", "-4294967296", "2147483647", "-2147483648",
    "0000000000000000000000001", "-0000000000000000000000001",
    "123456789012345678901234567890",
    "1234567", "12345678", "123456789", "1234567890123456", "12345678901234567",
    "-1234567", "-12345678", "-123456789", "-1234567890123456",
    "z1234567890", "1234567890z", "12345678z90", "\x7f" "9", "9\x7f",
    "/9", "9/", ":9", "9:", "9\xff", "\xff" "9",
};

fn check_parsers_malformed()
{
        for (positive i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
        {
                string_address input = malformed[i];

                same("string_to_positive", input, string_to_positive(input),
                     reference_trailing_positive(input));

                same("string_to_bipolar", input, (positive)string_to_bipolar(input),
                     (positive)reference_trailing_bipolar(input));
        }

        /*
                The same strings again at every offset into a word.

                A parser that reads eight bytes at a time reads the word the
                string ends in, and which bytes of that word are the string
                depends on where the string starts. Only one of the eight
                alignments is the one a literal happens to land on, so the
                other seven are where an off by one in the mask lives.
        */
        static b8 shifted[128];

        for (positive i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
                for (positive off = 0; off < 16; off++)
                {
                        positive at = 0;

                        while (malformed[i][at])
                        {
                                shifted[off + at] = malformed[i][at];
                                at++;
                        }

                        shifted[off + at] = 0;

                        same("string_to_positive", "at an offset",
                             string_to_positive(shifted + off),
                             reference_trailing_positive(shifted + off));

                        same("string_to_bipolar", "at an offset",
                             (positive)string_to_bipolar(shifted + off),
                             (positive)reference_trailing_bipolar(shifted + off));
                }

        // A run of digits longer than any word, at every length, so a parser
        // that takes eight at a time is entered with each possible remainder.
        static b8 run[64];

        for (positive length = 1; length < 40; length++)
        {
                for (positive i = 0; i < length; i++)
                        run[i] = (b8)('0' + (i * 7 + 3) % 10);

                run[length] = 0;

                same("string_to_positive", "a long run of digits",
                     string_to_positive(run), reference_trailing_positive(run));

                // and the same run with one byte in the middle spoiled, so the
                // wide path has to stop part way through a word
                for (positive where = 0; where < length; where++)
                {
                        b8 kept = run[where];

                        run[where] = 'x';

                        same("string_to_positive", "a run broken part way",
                             string_to_positive(run),
                             reference_trailing_positive(run));

                        run[where] = kept;
                }
        }
}

/*
        string_format past the six cases it had.

        The variadic ABI is the part that cannot be reasoned about from the
        outside: four integer registers on x86_64 and eight on the other two,
        then the stack, and a decimal counts against a different set. A format
        with five arguments in it reaches the stack path on x86_64 and nothing
        else here does.
*/
static b8 format_text[512];

fn expect(string_address detail, string_address want)
{
        same("string_format", detail, (positive)string_compare(caught, want), 0);
}

fn check_format_deep()
{
        catch_reset();
        string_format(catch_writer, (string_address) "");
        expect((string_address) "an empty format", (string_address) "");

        catch_reset();
        string_format(catch_writer, (string_address) "%%");
        expect((string_address) "one literal percent", (string_address) "%");

        catch_reset();
        string_format(catch_writer, (string_address) "%%%%%%");
        expect((string_address) "three of them", (string_address) "%%%");

        catch_reset();
        string_format(catch_writer, (string_address) "a%%b");
        expect((string_address) "a percent between text", (string_address) "a%b");

        // A percent that is the last byte writes nothing more, and an unknown
        // specifier is dropped with its percent. Both are relied on.
        catch_reset();
        string_format(catch_writer, (string_address) "tail%");
        expect((string_address) "a percent at the end", (string_address) "tail");

        catch_reset();
        string_format(catch_writer, (string_address) "a%zb");
        expect((string_address) "an unknown specifier", (string_address) "ab");

        catch_reset();
        string_format(catch_writer, (string_address) "%");
        expect((string_address) "a format that is one percent", (string_address) "");

        // Every argument kind, and then more of them than there are registers.
        catch_reset();
        string_format(catch_writer, (string_address) "%p", (positive)0);
        expect((string_address) "zero through the format", (string_address) "0");

        catch_reset();
        string_format(catch_writer, (string_address) "%p",
                      (positive)18446744073709551615ull);
        expect((string_address) "the top of positive",
               (string_address) "18446744073709551615");

        catch_reset();
        string_format(catch_writer, (string_address) "%b", (bipolar)-2147483647 - 1);
        expect((string_address) "the bottom of an int",
               (string_address) "-2147483648");

        catch_reset();
        string_format(catch_writer, (string_address) "%p %p %p %p %p %p %p %p",
                      (positive)1, (positive)2, (positive)3, (positive)4,
                      (positive)5, (positive)6, (positive)7, (positive)8);
        expect((string_address) "eight numbers, past every register",
               (string_address) "1 2 3 4 5 6 7 8");

        catch_reset();
        string_format(catch_writer,
                      (string_address) "%s %s %s %s %s %s %s %s",
                      "a", "b", "c", "d", "e", "f", "g", "h");
        expect((string_address) "eight strings, past every register",
               (string_address) "a b c d e f g h");

        catch_reset();
        string_format(catch_writer, (string_address) "%s%p%b%s%p%b%s%p%b",
                      "x", (positive)1, (bipolar)-1, "y", (positive)2, (bipolar)-2,
                      "z", (positive)3, (bipolar)-3);
        expect((string_address) "the kinds mixed, past every register",
               (string_address) "x1-1y2-2z3-3");

        // A format long enough that a scan taking eight bytes at a time runs
        // its loop several times, at every length either side of a word.
        for (positive length = 0; length < 40; length++)
        {
                for (positive i = 0; i < length; i++)
                        format_text[i] = (b8)('a' + i % 26);

                format_text[length] = 0;

                catch_reset();
                string_format(catch_writer, format_text);
                same("string_format", "plain text of every length",
                     (positive)string_compare(caught, format_text), 0);
        }

        /*
                A percent at every position in a run of text.

                A scan that takes a word at a time has to answer with the
                first percent in the word and not the last, and has to answer
                the terminator when both are in the same word. Only one
                position in eight is the one a hand written case lands on.
        */
        // The filler is spelled out rather than counted off the alphabet
        // because 's', 'p', 'b' and 'f' are specifiers: a percent landing in
        // front of one of those asks for an argument nobody passed, and the
        // routine reads whatever the register happened to hold. It did, and
        // the crash was in the writer rather than anywhere near the cause.
        static string_address filler = (string_address) "acdeghijklmnoqrtuvwxyz";

        for (positive length = 1; length < 40; length++)
                for (positive where = 0; where < length; where++)
                {
                        for (positive i = 0; i < length; i++)
                                format_text[i] = (b8)filler[i % 22];

                        format_text[where] = '%';
                        format_text[length] = 0;

                        // "%%" is a literal percent; anything else here is a
                        // specifier or a drop, and the expected answer is
                        // built the same way the routine has to build it.
                        positive at = 0;
                        static b8 expected[64];
                        positive out = 0;
                        b32 stop = 0;

                        while (format_text[at] && !stop)
                        {
                                if (format_text[at] != '%')
                                {
                                        expected[out++] = format_text[at++];
                                        continue;
                                }

                                if (!format_text[at + 1])
                                {
                                        stop = 1;
                                        break;
                                }

                                if (format_text[at + 1] == '%')
                                        expected[out++] = '%';

                                at += 2;
                        }

                        expected[out] = 0;

                        catch_reset();
                        string_format(catch_writer, format_text);
                        same("string_format", "a percent at every position",
                             (positive)string_compare(caught, expected), 0);
                }

        /*
                Text with the top bit set in it.

                A scan that hunts a zero byte and a percent a word at a time
                does its arithmetic on whole bytes, and 0x80 through 0xFF are
                where an approximation of that test goes wrong: a byte over
                0x80 answers the same as a zero byte to (w - 0x01..) & 0x80..
                unless the & ~w is there. Nothing else in this file puts one
                in a format string.
        */
        for (positive length = 1; length < 24; length++)
                for (positive where = 0; where < length; where++)
                {
                        for (positive i = 0; i < length; i++)
                                format_text[i] = (b8)filler[i % 22];

                        format_text[where] = (b8)(0x80 + (where * 7) % 128);
                        format_text[length] = 0;

                        catch_reset();
                        string_format(catch_writer, format_text);
                        same("string_format", "a byte over 0x7f in the text",
                             (positive)string_compare(caught, format_text), 0);

                        // and the same with a literal percent right after
                        // it, so the hunt has to answer the percent and not
                        // the high byte. Two of them, and the answer is the
                        // text with one percent where the pair was.
                        if (where + 2 < length)
                        {
                                static b8 expected_high[64];
                                positive out = 0;

                                format_text[where + 1] = '%';
                                format_text[where + 2] = '%';

                                for (positive i = 0; i < length; i++)
                                {
                                        if (i == where + 2)
                                                continue;

                                        expected_high[out++] = format_text[i];
                                }

                                expected_high[out] = 0;

                                catch_reset();
                                string_format(catch_writer, format_text);
                                same("string_format", "a high byte in front of a percent",
                                     (positive)string_compare(caught, expected_high), 0);
                        }
                }

        // The same text at every alignment, since a wide scan masks off what
        // sits before the string and the mask depends on where it starts.
        static b8 format_room[128];

        // Thirty two and not sixteen: where the hunt takes thirty two bytes
        // at a time it masks off what sits in front of the cursor by the low
        // five bits of it, and half the residues never came up at sixteen.
        for (positive off = 0; off < 32; off++)
                for (positive length = 0; length < 24; length++)
                {
                        for (positive i = 0; i < length; i++)
                                format_room[off + i] = (b8)('A' + i % 26);

                        format_room[off + length] = 0;

                        catch_reset();
                        string_format(catch_writer, format_room + off);
                        same("string_format", "text at every alignment",
                             (positive)string_compare(caught, format_room + off), 0);
                }
}

/*
        decimal_to_string on values that round awkwardly.

        The reference is the shape the assembly's own comment describes -- the
        sign, the whole part, a point, then six digits of fraction with the
        leading zeros written by hand -- because the C it replaced is gone and
        an approximation of it would only test itself. It is written here as
        one piece so that a change to the assembly has something to disagree
        with.
*/
fn reference_decimal_to_string(writer write, decimal value)
{
        if (0 > value)
        {
                write((address_any) "-", 1);
                value = -value;
        }

        bipolar whole = (bipolar)value;
        decimal fraction = value - (decimal)whole;

        if (whole < 0)
        {
                write((address_any) "-", 1);
                reference_positive_to_string(write, (positive)(-whole));
        }
        else
        {
                reference_positive_to_string(write, (positive)whole);
        }

        write((address_any) ".", 1);

        bipolar part = (bipolar)(fraction * 1000000.0);

        // Six digits, so the zeros a conversion would drop are written here.
        // The ladder stops at the first one that is not needed.
        if (part <= 99999)
        {
                write((address_any) "0", 1);

                if (part <= 9999)
                {
                        write((address_any) "0", 1);

                        if (part <= 999)
                        {
                                write((address_any) "0", 1);

                                if (part <= 99)
                                {
                                        write((address_any) "0", 1);

                                        if (part <= 9)
                                                write((address_any) "0", 1);
                                }
                        }
                }
        }

        if (part < 0)
        {
                write((address_any) "-", 1);
                part = -part;
        }

        reference_positive_to_string(write, (positive)part);
}

static b8 decimal_spelled[128];

fn one_decimal(decimal v, string_address detail)
{
        catch_reset();
        decimal_to_string(catch_writer, v);

        positive length = caught_length;

        for (positive i = 0; i <= length; i++)
                decimal_spelled[i] = caught[i];

        catch_reset();
        reference_decimal_to_string(catch_writer, v);

        same("decimal_to_string", detail, length, caught_length);
        same_bytes("decimal_to_string", detail, decimal_spelled, caught,
                   caught_length + 1);
}

/*
        The decimal specifier, which nothing above reaches.

        %f is the one argument kind that does not come out of the integer save
        area: it has its own cursor over its own set of registers, and on
        x86_64 a padded save area that the overflow is not padded to match.
        Every other case in this file either calls decimal_to_string directly
        or formats something that is not a decimal, so both of those arms had
        never run -- which is the shape of failure this file already carries a
        warning about further up.

        The answer to compare against is decimal_to_string's own, since that is
        what the specifier is defined to call.
*/
static b8 wanted_decimal[128];

fn one_format_decimal(decimal v, string_address detail)
{
        catch_reset();
        decimal_to_string(catch_writer, v);

        positive length = caught_length;

        for (positive i = 0; i <= length; i++)
                wanted_decimal[i] = caught[i];

        catch_reset();
        string_format(catch_writer, (string_address) "%f", v);

        same("string_format", detail, caught_length, length);
        same_bytes("string_format", detail, caught, wanted_decimal, length + 1);
}

static b8 expected_long[256];

/*
        A run of plain text long enough to reach the wide hunt, with a decimal
        argument still waiting.

        The wide hunt writes the vector registers the decimal arrived in. They
        were spilled in the prologue before any scan and the specifier reads
        them back out of the frame, so this is safe by the way the routine is
        built rather than by luck -- but every other case here has a run of one
        byte or none in front of its %f, so the two had never been in the same
        call.
*/
fn one_wide_decimal(string_address format, string_address text, decimal v)
{
        catch_reset();
        decimal_to_string(catch_writer, v);

        positive at = 0;

        while (text[at])
        {
                expected_long[at] = text[at];
                at++;
        }

        for (positive i = 0; i <= caught_length; i++)
                expected_long[at + i] = caught[i];

        catch_reset();
        string_format(catch_writer, format, v);

        same("string_format", "a wide run of text with a decimal waiting",
             (positive)string_compare(caught, expected_long), 0);
}

fn check_format_decimals()
{
        one_wide_decimal(
            (string_address) "a line of text long enough to go wide: %f",
            (string_address) "a line of text long enough to go wide: ", 1.5);
        one_wide_decimal(
            (string_address) "and one that rounds awkwardly on the end of it: %f",
            (string_address) "and one that rounds awkwardly on the end of it: ",
            0.0000005);
        one_wide_decimal(
            (string_address) "0123456789012345678901234567890123456789%f",
            (string_address) "0123456789012345678901234567890123456789", -2.25);

        static const decimal cases[] = {
            0.0, 1.5, -2.25, 0.0000005, -0.0000005, 123456.789, 1e6,
            3.14159265358979, 0.9999995, -1.0,
        };

        for (positive i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
                one_format_decimal(cases[i], (string_address) "one decimal");

        // Mixed with the other kinds, so the two cursors have to advance
        // past each other rather than in step.
        catch_reset();
        string_format(catch_writer, (string_address) "%s %f %p", "a", 1.5,
                      (positive)7);
        expect((string_address) "a decimal between the other kinds",
               (string_address) "a 1.500000 7");

        // and the other way round: the decimal first, so the run that goes
        // wide is scanned with the vector cursor already moved on
        catch_reset();
        decimal_to_string(catch_writer, 1.5);

        positive spelled_at = caught_length;

        for (positive i = 0; i <= spelled_at; i++)
                expected_long[i] = caught[i];

        {
                string_address rest =
                    (string_address) " then a run of text long enough to go wide";
                positive i = 0;

                while (rest[i])
                {
                        expected_long[spelled_at + i] = rest[i];
                        i++;
                }

                expected_long[spelled_at + i] = 0;
        }

        catch_reset();
        string_format(catch_writer,
                      (string_address) "%f then a run of text long enough to go wide",
                      1.5);
        same("string_format", "a wide run after a decimal",
             (positive)string_compare(caught, expected_long), 0);

        catch_reset();
        string_format(catch_writer, (string_address) "%f%s%f", 1.5, "|", -2.25);
        expect((string_address) "two decimals and a string",
               (string_address) "1.500000|-2.250000");

        /*
                More decimals than there are registers to pass them in.

                x86_64 has eight vector registers for this and arm64 has
                eight; riscv passes them in the integer ones, of which six are
                left after the writer and the format. So nine reaches the
                stack on all three, and the ninth is the first one that does
                on the two that have the most.
        */
        catch_reset();
        string_format(catch_writer,
                      (string_address) "%f %f %f %f %f %f %f %f %f",
                      1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
        expect((string_address) "nine decimals, past every register",
               (string_address) "1.000000 2.000000 3.000000 4.000000 5.000000 "
                                "6.000000 7.000000 8.000000 9.000000");

        catch_reset();
        string_format(catch_writer,
                      (string_address) "%f%f%f%f%f%f%f%f%f%f%f%f",
                      0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5,
                      10.5, 11.5);
        expect((string_address) "twelve decimals, well past them",
               (string_address) "0.5000001.5000002.5000003.5000004.500000"
                                "5.5000006.5000007.5000008.5000009.500000"
                                "10.50000011.500000");

        // and the kinds interleaved past both sets of registers at once
        catch_reset();
        string_format(catch_writer,
                      (string_address) "%p%f%s%p%f%s%p%f%s%p%f%s",
                      (positive)1, 1.5, "a", (positive)2, 2.5, "b",
                      (positive)3, 3.5, "c", (positive)4, 4.5, "d");
        expect((string_address) "every kind, past every register",
               (string_address) "11.500000a22.500000b33.500000c44.500000d");
}

fn check_decimals_deep()
{
        static const decimal awkward[] = {
            0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, -2.25, 2.675,
            0.1, 0.2, 0.3, 0.30000000000000004, 0.7, 0.9999995, -0.9999995,
            0.0000005, -0.0000005, 0.0000001, 0.9999999, 1.0000001,
            3.14159265358979, 2.71828182845905,
            123456.789, -123456.789, 999999.9999995, 1000000.0, -1000000.0,
            8388608.5, 16777216.25, 1e9, -1e9, 1e15, 4294967295.5,
            0.000001, 0.000009, 0.00001, 0.0001, 0.001, 0.01, 0.1,
            9007199254740992.0, 123.456789, 99.999999, 9.9999999,
        };

        for (positive i = 0; i < sizeof(awkward) / sizeof(awkward[0]); i++)
                one_decimal(awkward[i], (string_address) "a value that rounds awkwardly");

        // And a spread, so the fraction lands on every leading zero count
        for (positive i = 0; i < 512; i++)
        {
                decimal whole = (decimal)(bipolar)(next() % 1000000);
                decimal fraction = (decimal)(next() % 1000000) / 1000000.0;
                decimal v = whole + fraction;

                one_decimal(v, (string_address) "a spread of fractions");
                one_decimal(-v, (string_address) "a negative spread");
        }
}

/*
        Paths are byte operations with three unusually important boundaries:
        capacity zero must not even inspect its pointers, capacity one owns
        only the terminator, and a component can be much longer than the
        shell's 4096-byte path room. These references are the C bodies the
        assembly replaced, made truthfully bounded at zero and for dirname.
*/
#define PATH_ROOM 8256
#define PATH_GUARD 64

static p8 path_mine[PATH_ROOM];
static p8 path_want[PATH_ROOM];
static p8 path_source_a[PATH_ROOM];
static p8 path_source_b[PATH_ROOM];
static p8 path_page_a[3 * 4096];
static p8 path_page_b[3 * 4096];
static p8 path_written[PATH_ROOM];
static positive path_written_length;

static positive reference_path_join(p8 address_to into, positive capacity,
                                    string_address directory, string_address name)
{
        if (!capacity)
                return 0;

        positive length = reference_length(directory);

        if (length > capacity - 1)
                length = capacity - 1;

        for (positive i = 0; i < length; i++)
                into[i] = directory[i];

        if (length && into[length - 1] != '/' && length + 1 < capacity)
                into[length++] = '/';

        positive tail = reference_length(name);

        if (tail > capacity - 1 - length)
                tail = capacity - 1 - length;

        for (positive i = 0; i < tail; i++)
                into[length + i] = name[i];

        length += tail;
        into[length] = 0;
        return length;
}

static positive reference_path_tail(p8 address_to into, positive capacity,
                                    string_address path)
{
        if (!capacity)
                return 0;

        positive length = reference_length(path);

        while (length > 1 && path[length - 1] == '/')
                length--;

        positive start = length;

        if (!(length == 1 && path[0] == '/'))
                while (start && path[start - 1] != '/')
                        start--;
        else
                start = 0;

        positive found = length - start;

        if (found > capacity - 1)
                found = capacity - 1;

        for (positive i = 0; i < found; i++)
                into[i] = path[start + i];

        into[found] = 0;
        return found;
}

static positive reference_path_head(p8 address_to into, positive capacity,
                                    string_address path)
{
        if (!capacity)
                return 0;

        positive length = reference_length(path);

        while (length > 1 && path[length - 1] == '/')
                length--;

        positive cut = length;

        while (cut && path[cut - 1] != '/')
                cut--;

        if (!cut)
        {
                if (capacity > 1)
                {
                        into[0] = '.';
                        into[1] = 0;
                        return 1;
                }

                into[0] = 0;
                return 0;
        }

        while (cut > 1 && path[cut - 1] == '/')
                cut--;

        if (cut > capacity - 1)
                cut = capacity - 1;

        for (positive i = 0; i < cut; i++)
                into[i] = path[i];

        into[cut] = 0;
        return cut;
}

static fn path_test_writer(address_any data, positive length)
{
        for (positive i = 0; i < length; i++)
                path_written[i] = ((p8 address_to)data)[i];

        path_written_length = length;
        path_written[length] = 0;
}

static fn path_buffers_reset()
{
        reference_fill(path_mine, (b8)0xa5, PATH_ROOM);
        reference_fill(path_want, (b8)0xa5, PATH_ROOM);
}

static string_address path_place(p8 address_to room, positive offset,
                                 string_address text)
{
        positive length = reference_length(text);

        for (positive i = 0; i <= length; i++)
                room[offset + i] = text[i];

        return (string_address)(room + offset);
}

static fn path_tail_head_case(string_address path, positive capacity,
                              positive offset, string_address detail)
{
        p8 address_to got = path_mine + PATH_GUARD + offset;
        p8 address_to want = path_want + PATH_GUARD + offset;

        path_buffers_reset();
        positive got_length = path_tail_copy(got, capacity, path);
        positive want_length = reference_path_tail(want, capacity, path);
        same("path_tail_copy", detail, got_length, want_length);
        same_bytes("path_tail_copy", "guarded destination", path_mine,
                   path_want, PATH_ROOM);

        path_buffers_reset();
        got_length = path_head_copy(got, capacity, path);
        want_length = reference_path_head(want, capacity, path);
        same("path_head_copy", detail, got_length, want_length);
        same_bytes("path_head_copy", "guarded destination", path_mine,
                   path_want, PATH_ROOM);
}

static fn path_join_case(string_address directory, string_address name,
                         positive capacity, positive offset, string_address detail)
{
        p8 address_to got = path_mine + PATH_GUARD + offset;
        p8 address_to want = path_want + PATH_GUARD + offset;

        path_buffers_reset();
        positive got_length = path_join(got, capacity, directory, name);
        positive want_length = reference_path_join(want, capacity, directory, name);
        same("path_join", detail, got_length, want_length);
        same_bytes("path_join", "guarded destination", path_mine,
                   path_want, PATH_ROOM);
}

fn check_paths()
{
        static const string_address paths[] = {
            "",       "/",       "//",        "///",       "a",
            "a/",     "a//",     "/a",        "/a/",       "//a",
            "a/b",    "a//b",    "/a//b///",  ".",         "..",
            "./a",    "../a",    "alpha/beta", "alpha///beta////",
        };
        static const positive capacities[] = {0, 1, 2, 3, 7, 8, 4095, 4096};
        static const string_address directories[] = {
            "", "/", "//", "a", "a/", "a//", "/a", "/a/", "alpha/beta",
        };
        static const string_address names[] = {
            "", "b", "/b", "b/", "//b", "gamma/delta", "////",
        };

        // Capacity zero is stronger than a zero-byte result: no pointer is
        // inspected, so even null is a valid argument in this one case.
        same("path_join", "zero capacity null pointers",
             path_join(null, 0, null, null), 0);
        same("path_tail_copy", "zero capacity null pointers",
             path_tail_copy(null, 0, null), 0);
        same("path_head_copy", "zero capacity null pointers",
             path_head_copy(null, 0, null), 0);

        for (positive p = 0; p < sizeof(paths) / sizeof(paths[0]); p++)
        {
                for (positive c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++)
                        path_tail_head_case(paths[p], capacities[c], p & 7,
                                            "semantic corpus and capacity");

                reference_fill(path_written, (b8)0xa5, PATH_ROOM);
                path_written_length = positive_max;
                path_basename(path_test_writer, paths[p]);
                reference_fill(path_want, (b8)0xa5, PATH_ROOM);
                positive wanted = reference_path_tail(path_want, PATH_ROOM, paths[p]);
                same("path_basename", "shared split length", path_written_length,
                     wanted);
                same_bytes("path_basename", "shared split bytes", path_written,
                           path_want, wanted + 1);
        }

        for (positive i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
                for (positive c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++)
                        path_join_case(directories[i],
                                       names[(i + c) % (sizeof(names) / sizeof(names[0]))],
                                       capacities[c], (i + c) & 7,
                                       "slash and truncation corpus");

        // Every byte residue for all three pointers. The source offsets use
        // different odd strides, so the three alignments do not accidentally
        // stay congruent while each still visits all thirty two values.
        for (positive residue = 0; residue < 32; residue++)
        {
                string_address path = path_place(path_source_a, 64 + residue * 7 % 32,
                                                 "/alpha//beta///");
                path_tail_head_case(path, 64, residue, "every pointer residue");

                string_address directory =
                    path_place(path_source_a, 256 + residue * 11 % 32, "alpha/beta");
                string_address name =
                    path_place(path_source_b, 256 + residue * 13 % 32, "/gamma");
                path_join_case(directory, name, 64, residue, "every pointer residue");
        }

        // Overlong final component, overlong directory prefix, and both join
        // inputs overlong. These are the old dirname overflow and join/tail
        // truncation boundaries, with poison after the full 4096-byte room.
        for (positive i = 0; i < 6000; i++)
        {
                path_source_a[64 + i] = 'a';
                path_source_b[64 + i] = 'b';
        }
        path_source_a[64] = '/';
        path_source_a[64 + 6000] = 0;
        path_source_b[64 + 5000] = '/';
        path_source_b[64 + 6000] = 0;
        path_tail_head_case((string_address)(path_source_a + 64), 4096, 17,
                            "overlong final component");
        path_tail_head_case((string_address)(path_source_b + 64), 4096, 19,
                            "overlong directory prefix");
        path_join_case((string_address)(path_source_a + 64),
                       (string_address)(path_source_b + 64), 4096, 23,
                       "overlong directory and name");

        // Terminators immediately before and after page boundaries. The path
        // routines borrow string_length, whose aligned loads make these the
        // significant page positions even though this static test maps all
        // three pages so it is portable under every freestanding runner.
        positive base_a = (positive)(address_any)path_page_a;
        positive base_b = (positive)(address_any)path_page_b;
        positive edge_a = (4096 - (base_a & 4095)) & 4095;
        positive edge_b = (4096 - (base_b & 4095)) & 4095;
        if (!edge_a) edge_a = 4096;
        if (!edge_b) edge_b = 4096;

        string_address edge_text = "/page//edge///";
        positive edge_length = reference_length(edge_text);
        if (edge_a <= edge_length + 1) edge_a += 4096;
        if (edge_b < 5) edge_b += 4096;
        string_address edge_path = path_place(path_page_a,
                                              edge_a - edge_length - 1,
                                              edge_text);
        path_tail_head_case(edge_path, 4096, 29, "terminator at a page edge");

        string_address edge_directory = path_place(path_page_a,
                                                   edge_a + 1, "page/directory");
        string_address edge_name = path_place(path_page_b,
                                              edge_b - 5, "name");
        path_join_case(edge_directory, edge_name, 4096, 31,
                       "sources around page edges");
}

/*
        The shared numeric core, in one list.

        It runs twice: on its own, for the small cross-machine lane that a
        formatter change should not have to wait on unrelated platform tests
        for, and again as part of the full pass. Written out in both places it
        was two lists kept the same by hand, and a routine added to the full
        pass alone would quietly stop being checked on the machines that only
        have the small lane.
*/
fn check_formatters()
{
        check_into();
        check_padded();
        check_writer_fields();
        check_base_field();
        check_into_padded();
        check_into_pair();
        check_into_base();
        check_human_1024();
        check_human_nearest();
        check_wait_status_code();
        check_wait_status_code_base();
}

b32 main()
{
#if defined(VERIFY_FORMATTERS_ONLY)
        // A small cross-machine lane for the shared numeric core. It avoids
        // making a formatter change wait on unrelated platform tests when a
        // target is available only through a minimal linker and qemu-user.
        check_formatters();
#else
        check_fill();
        check_fill_wide();
        check_count();
        check_memory_compare();
#if RISCV64
        check_riscv_alignment_floor();
#endif
        check_memory_search();
        check_memory_search_ascii_case();
        check_find_long();
#if X64
        //      Once more down the narrow half. Every case above takes the
        //      wide body on this machine and would take it whatever was
        //      written under the test for the flag, so the flag is turned off
        //      and the whole set runs again.
        //
        //      Put back from a copy rather than by asking the processor
        //      again, so that nothing here has to know what the routine that
        //      asks is called this week.
        {
                p8 had_avx2 = cpu_has_avx2;
                p8 had_avx512 = cpu_has_avx512;

                cpu_has_avx2 = 0;
                cpu_has_avx512 = 0;
                check_memory_compare();
                check_memory_search();
                check_memory_search_ascii_case();
                cpu_has_avx2 = had_avx2;
                cpu_has_avx512 = had_avx512;
        }
#endif
        check_first_of_wide();
        check_hunts_wide();
        check_copy();
        check_exchange();
        check_frob();
        check_reverse();
        check_move();
        check_copy_fast_end();
        check_copy_end();
        check_strings();
        check_shell_set_cursor();
        check_string_edges();
        check_compare_edges();
        check_compare_wide();
        check_environment();
        check_bulk_strings();
        check_paths();
        check_bulk_numbers();
        check_numbers_exhaustive();
        check_numbers_edges();
        check_parsers_malformed();
        check_file_load();
        check_file_round_trip();
        check_lazy_file_and_library();
        check_memory();
        check_directory();
        check_clock();
        check_format();
        check_format_deep();
        check_decimals();
        check_decimals_deep();
        check_format_decimals();
#if X64
        //      And the format cases once more down the narrow half.
        //
        //      string_format hunts the end of a run of plain text
        //      thirty two bytes at a time where cpu_detect found AVX2
        //      and eight otherwise, and a machine has one of those, not
        //      both. Without this pass half the routine is the half a
        //      one byte error could live in forever.
        //
        //      Put back from a copy, as the pass at the top of this
        //      function is.
        {
                p8 had_avx2 = cpu_has_avx2;
                p8 had_avx512 = cpu_has_avx512;

                if (!had_avx2)
                        string_format(log, "  NOTE string_format: no AVX2 "
                                           "here, so the wide hunt was not "
                                           "the one tested\n");

                cpu_has_avx2 = 0;
                cpu_has_avx512 = 0;
                check_format();
                check_format_deep();
                check_format_decimals();
                cpu_has_avx2 = had_avx2;
                cpu_has_avx512 = had_avx512;
        }
#endif
        check_byte_helpers();
        check_span();
        check_lex_word();
        check_table_find();
        check_table_find_page_edge();
        check_hostile_neighbours();
        check_span_max();
        check_digits();
        check_signed_digits_and_width();
        check_digits_base();
        check_digits_exact();
        check_formatters();
        check_copy_max_end();
        check_bulk_alignments();
        check_bulk_moves();
        check_bulk_wide_strings();
#endif

        if (absent_count)
                string_format(log, "  %p routine(s) under neither name here\n",
                              absent_count);

        return test_report(null);
}
