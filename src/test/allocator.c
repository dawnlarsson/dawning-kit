/*
        Experimental C standard library

        The allocator, against the one on the machine

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        One file, built twice.

        Built the ordinary way it is the freestanding test: it includes
        compiler_memory.c and calls the allocator in src/standard/allocator.c,
        and it runs on x86_64, arm64 and riscv64. Built with
        ALLOCATOR_REFERENCE defined it links glibc instead and the same lines
        run through glibc's malloc.

        Everything printed before the "shared-end" line is a statement about
        behaviour rather than about addresses -- a count, a boolean, a
        checksum -- so the two builds must print those bytes identically, and
        src/test/allocator.sh diffs them. Anything printed after that line is
        about this allocator's own internals and has no reference to compare
        with.

        The torture loop is the part that matters. A linear congruential
        sequence with a fixed seed decides every size and every operation, so
        both builds walk the identical script; every live block carries a byte
        pattern derived from a tag that changes on every write, and every
        block is checked against its pattern before it is touched again. A
        realloc that loses a byte, a free list that hands the same block out
        twice, a class that reports more usable bytes than it has -- all three
        show up as a mismatch count that is not zero, on both sides, without
        either side knowing anything about the other's layout.
*/

#ifdef ALLOCATOR_REFERENCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

typedef unsigned char p8;
typedef unsigned long long positive;
typedef long long bipolar;
typedef int b32;
typedef void fn;

#define address_to *
#define address_of &
#define address_any void *
#define null ((void *)0)
#define bool p8

#define memory_copy_apart(destination, source, size) \
        memcpy((destination), (source), (size))

#define memory_zero(destination, size) memset((destination), 0, (size))

#else

#include "../compiler_memory.c"

#endif

//====================================================================
//      saying things, the same bytes either way
//====================================================================
/*
        Both builds fill one buffer and write it once at the end, so nothing
        about either standard output's buffering can reorder a line.
*/

static p8 say_buffer[1 << 16];
static positive say_used;

static fn say(const char address_to text)
{
        while (address_to text)
        {
                if (say_used < sizeof(say_buffer))
                        say_buffer[say_used++] = (p8)address_to text;

                text++;
        }
}

static fn say_number(positive value)
{
        p8 digits[24];
        b32 count = 0;

        if (!value)
                digits[count++] = '0';

        while (value)
        {
                digits[count++] = (p8)('0' + (value % 10));
                value /= 10;
        }

        while (count--)
                if (say_used < sizeof(say_buffer))
                        say_buffer[say_used++] = digits[count];
}

static fn say_line(const char address_to label, positive value)
{
        say(label);
        say(" ");
        say_number(value);
        say("\n");
}

static fn say_flush(void)
{
#ifdef ALLOCATOR_REFERENCE
        fwrite(say_buffer, 1, (size_t)say_used, stdout);
        fflush(stdout);
#else
        log(say_buffer, say_used);
        log_flush();
#endif
}

//====================================================================
//      the pattern every live block carries
//====================================================================

static positive check_failures;
static positive check_checks;

static fn check_fill(address_any block, positive size, positive tag)
{
        p8 address_to bytes = (p8 address_to)block;
        positive index = 0;

        while (index < size)
        {
                bytes[index] = (p8)(tag + index * 31);
                index++;
        }
}

static bool check_holds(address_any block, positive size, positive tag)
{
        p8 address_to bytes = (p8 address_to)block;
        positive index = 0;

        while (index < size)
        {
                if (bytes[index] != (p8)(tag + index * 31))
                        return 0;

                index++;
        }

        return 1;
}

static fn check_true(bool condition)
{
        check_checks++;

        if (!condition)
                check_failures++;
}

//====================================================================
//      the script both builds walk
//====================================================================

static positive check_seed = 0x2545F4914F6CDD1Dull;

static positive check_random(void)
{
        check_seed = check_seed * 6364136223846793005ull +
                     1442695040888963407ull;

        return check_seed >> 17;
}

#define CHECK_SLOTS 256
#define CHECK_TURNS 200000

static address_any check_live[CHECK_SLOTS];
static positive check_size[CHECK_SLOTS];
static positive check_tag[CHECK_SLOTS];

static positive check_allocations;
static positive check_frees;
static positive check_resizes;
static positive check_verifies;
static positive check_refused;

//      Every block this test is handed, whatever produced it, has to be
//      aligned for any type and has to say it holds at least what was asked.
static fn check_block(address_any block, positive size)
{
        check_true(((positive)block & 15) == 0);
        check_true(malloc_usable_size(block) >= size);
}

static positive check_size_wanted(void)
{
        //      Mostly small, because that is what programs do, with one
        //      request in sixty four large enough to leave the size classes
        //      entirely and become a mapping of its own.
        if ((check_random() & 63) == 0)
                return check_random() % 300000;

        return check_random() % 200;
}

static fn check_torture(void)
{
        positive turn = 0;
        positive tag = 1;

        while (turn < CHECK_TURNS)
        {
                positive slot = check_random() % CHECK_SLOTS;
                positive choice = check_random() % 100;

                turn++;

                if (!check_live[slot])
                {
                        positive size = check_size_wanted();
                        address_any block = malloc(size);

                        if (!block)
                        {
                                check_refused++;
                                continue;
                        }

                        check_allocations++;
                        check_block(block, size);
                        check_fill(block, size, tag);

                        check_live[slot] = block;
                        check_size[slot] = size;
                        check_tag[slot] = tag;
                        tag++;
                        continue;
                }

                check_verifies++;

                check_true(check_holds(check_live[slot], check_size[slot],
                                       check_tag[slot]));

                if (choice < 40)
                {
                        positive size = check_size_wanted();
                        positive keep = size < check_size[slot]
                                                ? size
                                                : check_size[slot];
                        address_any block =
                                realloc(check_live[slot], size);

                        if (size && !block)
                        {
                                check_refused++;
                                continue;
                        }

                        check_resizes++;

                        if (!size)
                        {
                                //      realloc to nothing frees and answers
                                //      null, so the slot is now empty.
                                check_true(block == null);
                                check_live[slot] = null;
                                continue;
                        }

                        check_block(block, size);

                        //      Whatever was there has to still be there, up
                        //      to the smaller of the two sizes.
                        check_true(check_holds(block, keep, check_tag[slot]));

                        check_fill(block, size, tag);
                        check_live[slot] = block;
                        check_size[slot] = size;
                        check_tag[slot] = tag;
                        tag++;
                        continue;
                }

                if (choice < 55)
                {
                        //      Overwrite in place, which is the case a free
                        //      list that hands one block out twice fails.
                        check_fill(check_live[slot], check_size[slot], tag);
                        check_tag[slot] = tag;
                        tag++;
                        continue;
                }

                free(check_live[slot]);
                check_frees++;
                check_live[slot] = null;
        }

        //      Everything still standing is checked once more and released,
        //      so a block corrupted late still counts.
        positive slot = 0;

        while (slot < CHECK_SLOTS)
        {
                if (check_live[slot])
                {
                        check_verifies++;

                        check_true(check_holds(check_live[slot],
                                               check_size[slot],
                                               check_tag[slot]));

                        free(check_live[slot]);
                        check_frees++;
                        check_live[slot] = null;
                }

                slot++;
        }
}

//====================================================================
//      the corners
//====================================================================

#define CHECK_HELD 64

static address_any check_held[CHECK_HELD];
static positive check_held_size[CHECK_HELD];
static positive check_held_tag[CHECK_HELD];

static fn check_corners(void)
{
        //      A request of nothing still answers with a pointer free takes,
        //      and two of them are two different blocks.
        address_any nothing_one = malloc(0);
        address_any nothing_two = malloc(0);

        check_true(nothing_one != null);
        check_true(nothing_two != null);
        check_true(nothing_one != nothing_two);
        free(nothing_one);
        free(nothing_two);

        //      Freeing nothing does nothing.
        free(null);

        //      A resize from nothing is an allocation.
        address_any grown = realloc(null, 100);

        check_true(grown != null);
        check_block(grown, 100);
        check_fill(grown, 100, 7);

        //      Growing preserves, shrinking preserves what is left.
        grown = realloc(grown, 4000);
        check_true(grown != null);
        check_true(check_holds(grown, 100, 7));
        check_block(grown, 4000);

        grown = realloc(grown, 40);
        check_true(grown != null);
        check_true(check_holds(grown, 40, 7));

        //      And a resize to nothing frees it and answers null.
        check_true(realloc(grown, 0) == null);

        //      Zeroed memory, from a block that was certainly dirty first.
        address_any dirty = malloc(600);

        check_true(dirty != null);
        check_fill(dirty, 600, 200);
        free(dirty);

        positive round = 0;

        while (round < 32)
        {
                address_any clean = calloc(600, 1);
                positive index = 0;
                bool zero = 1;

                check_true(clean != null);

                while (index < 600)
                {
                        if (((p8 address_to)clean)[index])
                                zero = 0;

                        index++;
                }

                check_true(zero);
                check_fill(clean, 600, 99);
                free(clean);
                round++;
        }

        //      A large calloc, which on a fresh mapping is allowed to skip
        //      the store loop entirely and must still read back as zero.
        address_any wide = calloc(400000, 1);
        positive index = 0;
        bool zero = 1;

        check_true(wide != null);

        while (index < 400000)
        {
                if (((p8 address_to)wide)[index])
                        zero = 0;

                index += 7;
        }

        check_true(zero);
        free(wide);

        //      The multiply that must not wrap.
        check_true(calloc((positive)-1, 2) == null);
        check_true(calloc(2, (positive)-1) == null);
        check_true(calloc((positive)1 << 62, 8) == null);

        //      Zero elements is a legal allocation of nothing.
        address_any none = calloc(0, 8);

        check_true(none != null);
        free(none);

        none = calloc(8, 0);
        check_true(none != null);
        free(none);

        //      Nothing has a usable size of nothing.
        check_true(malloc_usable_size(null) == 0);

        //      Alignments, every power of two the page and under.
        positive alignment = 16;

        while (alignment <= 65536)
        {
                address_any placed = aligned_alloc(alignment, alignment * 3);

                check_true(placed != null);
                check_true(((positive)placed & (alignment - 1)) == 0);
                check_true(malloc_usable_size(placed) >= alignment * 3);
                check_fill(placed, alignment * 3, 11);
                check_true(check_holds(placed, alignment * 3, 11));
                free(placed);

                alignment += alignment;
        }

        //      The older interface, and the codes it answers with. glibc
        //      refuses anything that is not a power of two at least as wide
        //      as a pointer, and this table is the whole of that rule.
        static const positive asked[] = {0, 1, 2, 3, 4, 8, 16, 24, 32, 64,
                                         4096, 65536};
        positive which = 0;

        while (which < sizeof(asked) / sizeof(asked[0]))
        {
                address_any placed = (address_any)1;
                b32 answer = posix_memalign(address_of placed, asked[which],
                                            300);

                say("memalign ");
                say_number(asked[which]);
                say(" ");
                say_number((positive)answer);
                say("\n");

                if (!answer)
                {
                        check_true(((positive)placed &
                                    (asked[which] - 1)) == 0);
                        check_true(malloc_usable_size(placed) >= 300);
                        free(placed);
                }

                which++;
        }

        //      A mapping grown and shrunk across the whole range, with the
        //      contents checked at every step. This is the path mremap is on.
        positive want = 1 << 20;
        address_any big = malloc(want);

        check_true(big != null);
        check_fill(big, want, 55);

        big = realloc(big, 4 << 20);
        check_true(big != null);
        check_true(check_holds(big, want, 55));
        check_block(big, 4 << 20);

        big = realloc(big, 300000);
        check_true(big != null);
        check_true(check_holds(big, 300000 < want ? 300000 : want, 55));

        big = realloc(big, 900);
        check_true(big != null);
        check_true(check_holds(big, 900, 55));
        check_block(big, 900);

        big = realloc(big, 8 << 20);
        check_true(big != null);
        check_true(check_holds(big, 900, 55));
        check_block(big, 8 << 20);

        free(big);

        //      A grow-by-one loop, which is the shape that a realloc deciding
        //      badly turns into a copy every turn.
        address_any creep = malloc(1);
        positive length = 1;

        check_true(creep != null);
        check_fill(creep, 1, 3);

        while (length < 5000)
        {
                positive next = length + 1;
                address_any moved = realloc(creep, next);

                check_true(moved != null);
                check_true(check_holds(moved, length, 3));
                ((p8 address_to)moved)[length] = (p8)(3 + length * 31);
                creep = moved;
                length = next;
        }

        check_true(check_holds(creep, length, 3));
        free(creep);

        /*
                Every size across the class boundaries, sixty four blocks held
                at a time, each written to the very last byte it claims to
                have.

                Sixty four and not one. A block filled to its usable size with
                nothing else live writes into memory no other block owns yet,
                so a usable size that over-reported by a word would read back
                perfectly and nothing would ever know. With sixty three
                neighbours standing, the same word lands in one of them and
                comes back as a mismatch -- on either allocator, because
                glibc's malloc_usable_size is exact too.
        */
        positive filled = 0;
        positive size = 0;

        while (size <= 70000)
        {
                address_any block = malloc(size);

                check_true(block != null);
                check_block(block, size);

                check_held[filled] = block;
                check_held_size[filled] = malloc_usable_size(block);
                check_held_tag[filled] = size + 1;
                check_fill(block, check_held_size[filled],
                           check_held_tag[filled]);
                filled++;

                if (filled == CHECK_HELD)
                {
                        positive which = 0;

                        while (which < CHECK_HELD)
                        {
                                check_true(check_holds(
                                        check_held[which],
                                        check_held_size[which],
                                        check_held_tag[which]));

                                free(check_held[which]);
                                which++;
                        }

                        filled = 0;
                }

                size = size < 300 ? size + 1 : size + 97;
        }

        while (filled)
        {
                filled--;

                check_true(check_holds(check_held[filled],
                                       check_held_size[filled],
                                       check_held_tag[filled]));

                free(check_held[filled]);
        }
}

//====================================================================
//      what only this allocator can be asked
//====================================================================

#ifndef ALLOCATOR_REFERENCE
static fn check_own(void)
{
        positive own_failures = 0;

        //      Every shelf is a multiple of the alignment and every shelf is
        //      larger than the one before it.
        b32 class = 0;

        while (class < ALLOCATOR_CLASSES)
        {
                if (allocator_class_size[class] % ALLOCATOR_ALIGNMENT)
                        own_failures++;

                if (class && allocator_class_size[class] <=
                                     allocator_class_size[class - 1])
                        own_failures++;

                class++;
        }

        say_line("classes", ALLOCATOR_CLASSES);
        say_line("class-table-faults", own_failures);
        check_true(own_failures == 0);

        //      The arithmetic in allocator_class_of, against the table it is
        //      not allowed to look at: for every request the shelf it names
        //      has to be large enough, and the shelf below it has to be too
        //      small, which is what makes it the smallest one that fits.
        positive want = 1;
        positive lookup_failures = 0;

        while (want <= ALLOCATOR_LARGEST)
        {
                b32 found = allocator_class_of(want);

                if (found >= ALLOCATOR_CLASSES)
                        lookup_failures++;
                else
                {
                        if (allocator_class_size[found] < want)
                                lookup_failures++;

                        if (found && allocator_class_size[found - 1] >= want)
                                lookup_failures++;
                }

                want++;
        }

        say_line("lookup-faults", lookup_failures);
        say_line("lookup-above-largest",
                 (positive)allocator_class_of(ALLOCATOR_LARGEST + 1));
        check_true(lookup_failures == 0);
        check_true(allocator_class_of(ALLOCATOR_LARGEST + 1) ==
                   ALLOCATOR_CLASSES);

        //      What the smallest requests actually cost, which is the number
        //      the eight byte header exists for.
        address_any block = malloc(0);
        say_line("usable-of-0", malloc_usable_size(block));
        free(block);

        block = malloc(8);
        say_line("usable-of-8", malloc_usable_size(block));
        free(block);

        block = malloc(24);
        say_line("usable-of-24", malloc_usable_size(block));
        free(block);

        block = malloc(1000);
        say_line("usable-of-1000", malloc_usable_size(block));
        free(block);

        //      A freed block of a shelf comes straight back for the next
        //      request of that shelf, which is the whole point of the lists.
        address_any first = malloc(100);
        free(first);
        address_any again = malloc(100);

        say_line("reused", (positive)(first == again));
        check_true(first == again);
        free(again);

        //      Sizes inside one shelf do not move the block.
        address_any held = malloc(100);
        address_any same = realloc(held, 96);

        say_line("resize-in-place", (positive)(held == same));
        check_true(held == same);
        free(same);

        /*
                A second free of the same pointer, which cannot be asked of
                glibc in the shared half above because glibc answers it by
                killing the program. Here it is a no-op, and the proof is that
                the next two allocations of that shelf are two blocks and not
                one: without the freed tag the list would point at the block
                twice and hand the same address out twice.
        */
        address_any twice = malloc(48);

        free(twice);
        free(twice);

        address_any one = malloc(48);
        address_any two = malloc(48);

        say_line("double-free-distinct", (positive)(one != two));
        check_true(one != two);
        free(one);
        free(two);

        //      And a pointer this allocator never handed out, with a word in
        //      front of it that is not a tag it wrote. Nothing is written and
        //      no list is touched.
        static positive stranger[8];

        stranger[0] = 0x5AFE5AFE5AFE5AFEull;
        free(address_of stranger[1]);

        say_line("stranger-untouched",
                 (positive)(stranger[0] == 0x5AFE5AFE5AFE5AFEull));
        check_true(stranger[0] == 0x5AFE5AFE5AFE5AFEull);
}
#endif

b32 main(void)
{
        check_torture();
        check_corners();

        say_line("allocations", check_allocations);
        say_line("frees", check_frees);
        say_line("resizes", check_resizes);
        say_line("verifies", check_verifies);
        say_line("refused", check_refused);
        say_line("failures", check_failures);
        say("shared-end\n");

#ifndef ALLOCATOR_REFERENCE
        check_own();
#endif

        //      The verdict in the shape src/test/run reads, after the line
        //      the glibc diff stops at so that the two builds are allowed to
        //      have counted different numbers of checks.
        say_number(check_checks);
        say(" checks, ");
        say_number(check_failures);
        say(" failures\n");

        say_flush();
        return check_failures ? 1 : 0;
}
