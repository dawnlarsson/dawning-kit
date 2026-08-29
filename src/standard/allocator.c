/*
        Experimental C standard library

        The allocator: malloc, free, calloc, realloc, and the aligned pair

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_ALLOCATOR
#define STANDARD_MODERN_C_STANDARD_ALLOCATOR

/*
        Nothing here in a kernel build, and nothing here on Windows.

        src/core.c defines STANDARD_MODERN_C_KERNEL and then includes
        compiler_memory.c, so everything in this file would otherwise be
        compiled into the module, where mmap is not a thing that can be
        called and where a symbol named free would be a very bad idea. A
        module allocates with kmalloc and vmalloc and always has.

        Windows is out for the plainer reason that memory() and memory_free()
        -- the mmap pair this stands on -- are themselves inside library.c's
        "not Windows" guard, so there is nothing underneath to build on.
*/
#if !defined(KERNEL_MODE) && !defined(WINDOWS)

/*
        This is ordinary C on purpose, for the same reason netlink.c is.

        library.c and everything it includes holds declarations and assembly
        and nothing else, and that is checked. An allocator is not a floor. It
        is policy: which shelf a size goes on, when a chunk is asked of the
        kernel, whether a block that shrank is worth moving. None of that is a
        thing one machine does differently from another, so it is written once
        here rather than three times there, and the same bytes are what run on
        x86_64, arm64 and riscv64.

        What it stands on is the assembly. memory() is mmap and memory_free()
        is munmap, both already written for all three; memory_copy_apart is
        memcpy and memory_zero is the store loop; bits_leading_zeros is one
        instruction on two of the three targets and a six step fold on the
        third. Nothing below reimplements any of them.

        THE CONTRACT PROBLEM, WHICH IS THE WHOLE DESIGN

        memory_free(address, size) is munmap and munmap wants the length. The
        caller of free(p) has one pointer and no length, and by the time it
        calls it has usually forgotten there ever was one. So the length has
        to be written down somewhere the pointer can reach, which means a
        header, which means the returned pointer is not the start of what was
        allocated.

        That is where alignment bites. malloc must return something suitable
        for any type the program might put there, which on all three of these
        targets is sixteen bytes -- long double on x86_64, and the pair
        instructions on arm64 and riscv64 want it even where the ABI would
        settle for eight. The obvious reading is that the header must
        therefore be sixteen bytes too, and that reading is wrong, and paying
        it costs half of every small allocation.

        What has to be a multiple of sixteen is the STRIDE, not the header. If
        blocks start at addresses that are eight modulo sixteen and every
        block size is a multiple of sixteen, then base plus eight is sixteen
        aligned for every block in the chunk, forever, and the header is one
        word. So that is the layout:

              block base   ->  [ tag ][ payload ..................... ]
                                 8 b    size - 8 bytes, 16 byte aligned

        The tag sits at payload minus eight and says what the block is. Two
        kinds of block need a second word -- a mapping has to remember how
        many bytes to hand back to munmap, and an over-aligned block has to
        remember where the real allocation started -- and those two put it at
        payload minus sixteen, in front of the tag:

              [ extra ][ tag ][ payload ......... ]     mapped, or shifted

        Reading is still one load at payload minus eight in every case, and
        the second word is only reached after the tag has already said it is
        there. glibc arrives at the same place from the other direction: its
        chunk header is two words, but one of them is the previous chunk's
        payload, so a live small chunk also costs eight.

        WHERE THIS CAME FROM

        src/sh/awk.c has had a working size class allocator in it for a while:
        awk_take and awk_give, twenty two power of two classes, the class in
        the eight bytes before the block, a bump pointer for fresh chunks and
        one free list per class. This is that allocator promoted, and it is
        worth being exact about what changed, because the changes are the
        difference between something awk can live with and something every
        program in the tree has to live with.

          - awk returns block + 8 from a block whose base is a power of two
            multiple, so its payloads are eight byte aligned and not sixteen.
            awk only ever puts characters and its own structures there and
            never noticed. A malloc cannot ship that, hence the stride above.

          - awk's classes are powers of two, so a 40 byte structure occupies
            64 bytes and a 1025 byte buffer occupies 2048. Here each power of
            two is cut into quarters above 64 -- 64 80 96 112 128 160 192 224
            and so on -- which caps the rounding waste at 25% instead of just
            under 100%, for one extra shift in the lookup and 52 free list
            heads instead of 22.

          - awk calls awk_out_of_memory and exits. A library returns null.

          - awk abandons whatever is left of a chunk when the next request
            does not fit. Here the remainder is cut into the largest classes
            that fit and pushed onto their free lists, so a chunk boundary
            costs nothing at all.

          - awk's chunk is four megabytes, always, so a program that allocates
            one string pays four megabytes of address space for it. Here the
            first chunk is 64 KiB and each next one doubles to a ceiling of
            four megabytes, which is the usual shape and costs one variable.

          - The free list link overwrites awk's class word, so a block on the
            list has forgotten what it is until it is popped again. Here the
            link lives in the payload -- every class is at least sixteen bytes
            so there are always at least eight to put it in -- and the tag is
            true at every moment of a block's life. That is what makes it safe
            for free() to look at the tag and refuse a block whose tag is not
            one it wrote.

        THREADS

        There are none. This allocator is single threaded and there is no lock
        anywhere in it: two threads calling malloc at the same time will
        corrupt the free lists, and two threads calling free on blocks of the
        same class will lose one of them. That is a deliberate choice for now
        and not an oversight -- nothing in the tree runs a second thread
        through it, and an atomic on every allocation is a real cost to pay
        for a case that does not exist yet.

        When it does exist, the whole of the mutable state is the six file
        scope objects declared below: the free list array, the bump pointer,
        what is left beside it, and the next chunk size. Every one of them is
        touched only inside allocator_take and memory_give. A single lock
        taken at the top of those two, or a per class list made atomic with a
        tagged head, is the whole of the work, and the layout above does not
        change either way.

        WHAT IS NEVER GIVEN BACK

        Chunk memory is never unmapped. A block freed goes onto its class's
        free list and waits there for a request of that class; it is not
        coalesced with its neighbours, because there are no neighbour links to
        coalesce with and adding them would put a second word back into every
        small block. So the high water mark of each class is held for the life
        of the process. Blocks too big for any class are their own mapping and
        those are handed straight back to the kernel on free, which is where
        the memory a long running program actually notices lives.
*/

//      The smallest alignment malloc may return. Sixteen on all three of
//      these targets and not likely to grow; if it ever does, every class
//      size below has to become a multiple of the new number.
#define ALLOCATOR_ALIGNMENT 16

//      One word in front of the payload for a class block, two for the two
//      kinds that carry a second word. The wide one is what a mapping's base
//      is offset by, so it also keeps the payload sixteen aligned.
#define ALLOCATOR_HEADER 8
#define ALLOCATOR_HEADER_WIDE 16

/*
        Four kilobytes, and it is a rounding unit rather than a claim about
        the machine. arm64 is configured with sixteen or sixty four kilobyte
        pages on plenty of real systems, and this number being smaller than
        the true page is harmless in both directions: mmap rounds a length up
        on its own, and munmap rounds a length up to the same true page, so a
        mapping recorded as a multiple of four kilobytes is unmapped exactly
        and completely.

        That holds for mremap too, and mremap is the one that would hurt if it
        did not: rounding a length up to four kilobytes can never cross a
        larger page boundary that the true rounding would not have crossed,
        because the true rounding of a length is itself a multiple of four
        kilobytes and is at least the length, so the four kilobyte rounding
        always lands at or below it and both then round to the same page. A
        length that came out short of the real mapping would make mremap move
        the front of a mapping and leave the tail of it behind, and that is
        the failure this paragraph exists to rule out.

        The only consequence is that malloc_usable_size reports less than the
        kernel really left there on a machine with larger pages, which is the
        safe direction to be wrong in.
*/
#define ALLOCATOR_PAGE 4096

//      The first chunk asked of the kernel, and the largest. Doubling from
//      one to the other means a program that allocates a handful of strings
//      touches 64 KiB of address space and a program that allocates a million
//      of them stops asking after a dozen calls.
#define ALLOCATOR_CHUNK_FIRST (64u << 10)
#define ALLOCATOR_CHUNK_MAX (4u << 20)

/*
        A ceiling on any single size this will consider, so that adding a
        header, an alignment's worth of padding and a page of rounding to it
        cannot wrap. It is a sixteenth of the address space, which is 2^60 on
        these three and 2^28 where positive is thirty two bits wide, and in
        both cases it is far past anything the kernel would map anyway. The
        point is not the number, the point is that every sum below is proven
        not to overflow by one comparison at the top.
*/
#define ALLOCATOR_LIMIT (((positive)-1) >> 4)

/*
        Fifty two shelves. Sixteen, thirty two and forty eight on their own,
        and from sixty four upward each power of two cut into quarters, up to
        a quarter of a megabyte. Everything above that is its own mapping.

        Every one of them is a multiple of sixteen, which is what keeps the
        payloads aligned as the bump pointer walks a chunk, and the table is
        written out rather than computed so that the arithmetic in
        allocator_class_of can be checked against something that is not
        itself.
*/
#define ALLOCATOR_CLASSES 52
#define ALLOCATOR_LARGEST 262144

//      Tags that are not class indexes. Both are just past the shelves, so a
//      single unsigned compare separates a live block of a class from
//      everything else.
#define ALLOCATOR_MAPPED ((positive)ALLOCATOR_CLASSES)
#define ALLOCATOR_SHIFTED ((positive)ALLOCATOR_CLASSES + 1)

/*
        And a whole band of tags above those, one per shelf, that mean "on the
        free list of this shelf right now".

        This is what the link living in the payload buys. awk's allocator puts
        the free list link where the class word is, so a block on the list has
        no tag at all and a second free of the same pointer walks straight
        through and pushes it a second time -- after which two allocations of
        that shelf return the same address and the program has two owners of
        one block and no way to find out. Here the tag is a separate word that
        nothing on the list path needs, so freeing can move it into this band
        and taking can move it back, and a second free finds a tag that is not
        a shelf and does nothing at all.

        It is not a heap checker. It does not notice a pointer into the middle
        of a block, or a write that ran off the end of the block before it, or
        a free of a stack address that happens to have a small number eight
        bytes in front of it. What it does is turn the single most common
        memory bug in C from silent list corruption into nothing happening,
        for one addition on each side.
*/
#define ALLOCATOR_FREED ((positive)ALLOCATOR_CLASSES + 2)

//      mremap's flag word: the kernel may pick a new address rather than
//      failing when the mapping cannot grow where it stands.
#define ALLOCATOR_MREMAP_MAYMOVE 1

//      What posix_memalign answers with. It reports through its return value
//      rather than through errno, which is the one place in the C library
//      where that is true, and it is why this file needs no errno at all.
#define ALLOCATOR_EINVAL 22
#define ALLOCATOR_ENOMEM 12

static const positive allocator_class_size[ALLOCATOR_CLASSES] = {
        16, 32, 48,
        64, 80, 96, 112,
        128, 160, 192, 224,
        256, 320, 384, 448,
        512, 640, 768, 896,
        1024, 1280, 1536, 1792,
        2048, 2560, 3072, 3584,
        4096, 5120, 6144, 7168,
        8192, 10240, 12288, 14336,
        16384, 20480, 24576, 28672,
        32768, 40960, 49152, 57344,
        65536, 81920, 98304, 114688,
        131072, 163840, 196608, 229376,
        262144,
};

//      One head per shelf, holding payload addresses rather than bases. The
//      link to the next free block lives in the payload's first eight bytes,
//      which every class has room for, so a pop is a load and a store and the
//      tag is never disturbed.
static address_any allocator_free_list[ALLOCATOR_CLASSES];

//      The base of the next block to be cut from the current chunk, which is
//      always eight modulo sixteen, and how many bytes are left after it.
static p8 address_to allocator_bump;
static positive allocator_bump_left;

//      How large the next chunk asked of the kernel will be. Zero means none
//      has been asked for yet, which is what a program that never allocates
//      pays: three words of bss and no syscall.
static positive allocator_chunk_next;

//      The address of the tag, and of the second word the two wide kinds put
//      in front of it. Written as functions returning the address rather than
//      as macros returning the value so that both reading and writing them
//      read the same at the call site.
static positive address_to allocator_tag(address_any block)
{
        return (positive address_to)block - 1;
}

static positive address_to allocator_extra(address_any block)
{
        return (positive address_to)block - 2;
}

//      The free list link, which is the payload's own first word.
static address_any address_to allocator_link(address_any block)
{
        return (address_any address_to)block;
}

static positive allocator_page_round(positive bytes)
{
        return (bytes + (ALLOCATOR_PAGE - 1)) & ~(positive)(ALLOCATOR_PAGE - 1);
}

/*
        Which shelf a request of this many bytes -- header included -- belongs
        on, or ALLOCATOR_CLASSES when it belongs on none of them.

        The three smallest are answered outright because the quarter cut has
        no meaning below sixty four: a quarter of thirty two is eight and the
        stride would stop being sixteen. From sixty five upward the answer is
        arithmetic. Take the position of the highest set bit, call it high, so
        that the request sits in [2^high, 2^(high+1)). A quarter of that
        interval is 2^(high-2), and rounding the request up to a whole number
        of quarters gives a value from four to eight. Four through seven are
        the four shelves of this interval; eight is the next interval's first
        shelf, and the index arithmetic below lands on it without a branch,
        because the four shelves of every interval are consecutive.

        Which is the reason the shelves above sixty four are laid out in the
        table in groups of four in the first place.
*/
static b32 allocator_class_of(positive want)
{
        if (want > ALLOCATOR_LARGEST)
                return ALLOCATOR_CLASSES;

        if (want <= 16)
                return 0;

        if (want <= 32)
                return 1;

        if (want <= 48)
                return 2;

        if (want <= 64)
                return 3;

        b32 high = 63 - bits_leading_zeros(want);
        positive step = (positive)1 << (high - 2);
        positive quarter = (want + step - 1) >> (high - 2);

        return 3 + 4 * (high - 6) + (b32)(quarter - 4);
}

/*
        How many bytes a fresh allocation of this size would actually leave
        usable. realloc asks this rather than comparing sizes, because the
        question it needs answered is not "is the new size smaller" but "would
        a new block be a different block at all" -- and inside one shelf the
        answer is no, whichever direction the size moved.
*/
static positive allocator_fit(positive bytes)
{
        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        if (class < ALLOCATOR_CLASSES)
                return allocator_class_size[class] - ALLOCATOR_HEADER;

        return allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE) -
               ALLOCATOR_HEADER_WIDE;
}

/*
        Spend what is left of the current chunk before abandoning it.

        A chunk ends when the next request does not fit in what remains, and
        what remains at that moment is anything from nothing to one byte short
        of the largest class. Cutting it into the biggest shelves that fit and
        pushing those onto their free lists turns the whole of it back into
        allocations, so the only memory a chunk boundary loses is whatever is
        left under sixteen bytes.

        allocator_class_of rounds up, so the shelf it names for the remainder
        is either exactly the remainder or one too big, and stepping back one
        is enough. The loop is bounded by fifty two iterations because each
        turn takes at least sixteen bytes and each next shelf is no larger
        than the one before.
*/
static fn allocator_spend_remainder(void)
{
        while (allocator_bump_left >= allocator_class_size[0])
        {
                b32 class = allocator_class_of(allocator_bump_left);

                if (class >= ALLOCATOR_CLASSES)
                        class = ALLOCATOR_CLASSES - 1;
                else if (allocator_class_size[class] > allocator_bump_left)
                        class--;

                address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

                address_to allocator_tag(block) = ALLOCATOR_FREED + class;
                address_to allocator_link(block) = allocator_free_list[class];
                allocator_free_list[class] = block;

                allocator_bump += allocator_class_size[class];
                allocator_bump_left -= allocator_class_size[class];
        }
}

/*
        The one place a block comes from.

        fresh, when a caller passes an address for it, comes back true only
        when the payload is known to be untouched kernel memory and therefore
        already zero. That is exactly two cases: a block cut from the bump
        pointer, because a chunk is freshly mapped and the bump pointer only
        ever moves forward over it, and a mapping of its own. A block off a
        free list has been written by whoever had it last and says false, and
        so does a block cut from a chunk's remainder, because pushing it onto
        a free list wrote a link into its payload. calloc is the only caller
        that asks, and the only thing it does with a false is zero the block
        it would otherwise have had to zero anyway.
*/
static address_any allocator_take(positive bytes, bool address_to fresh)
{
        if (fresh)
                address_to fresh = 0;

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        //      Too big for any shelf: its own mapping, and the length written
        //      down in front of the tag because munmap will want it back.
        if (class >= ALLOCATOR_CLASSES)
        {
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive got = (positive)memory(whole);

                //      memory() is the raw trap and returns the kernel's
                //      answer unchanged, so a failure is a small negative
                //      number wearing an unsigned hat.
                if (!got || got >= (positive)-4095)
                        return null;

                address_any block = (address_any)(got + ALLOCATOR_HEADER_WIDE);

                address_to allocator_tag(block) = ALLOCATOR_MAPPED;
                address_to allocator_extra(block) = whole;

                if (fresh)
                        address_to fresh = 1;

                return block;
        }

        if (allocator_free_list[class])
        {
                address_any block = allocator_free_list[class];

                allocator_free_list[class] = address_to allocator_link(block);

                //      Back from the freed band to the plain shelf number,
                //      which is what says this block is live.
                address_to allocator_tag(block) = (positive)class;

                return block;
        }

        positive size = allocator_class_size[class];

        if (allocator_bump_left < size)
        {
                allocator_spend_remainder();

                positive chunk = allocator_chunk_next;

                if (!chunk)
                        chunk = ALLOCATOR_CHUNK_FIRST;

                //      A shelf larger than the chunk schedule has reached
                //      gets a chunk of its own size instead, rather than the
                //      schedule being jumped forward for one request.
                if (chunk < size + ALLOCATOR_HEADER)
                        chunk = allocator_page_round(size + ALLOCATOR_HEADER);

                positive got = (positive)memory(chunk);

                if (!got || got >= (positive)-4095)
                        return null;

                //      Eight bytes of the page go unused so that the first
                //      base lands eight past a sixteen byte boundary, which
                //      is what puts every payload in the chunk on one.
                allocator_bump = (p8 address_to)(got + ALLOCATOR_HEADER);
                allocator_bump_left = chunk - ALLOCATOR_HEADER;

                allocator_chunk_next = chunk < ALLOCATOR_CHUNK_MAX
                                               ? chunk + chunk
                                               : ALLOCATOR_CHUNK_MAX;

                if (allocator_chunk_next > ALLOCATOR_CHUNK_MAX)
                        allocator_chunk_next = ALLOCATOR_CHUNK_MAX;
        }

        address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

        allocator_bump += size;
        allocator_bump_left -= size;

        address_to allocator_tag(block) = (positive)class;

        if (fresh)
                address_to fresh = 1;

        return block;
}

/*
        malloc.

        A request of zero is a request for a block: the standard allows null
        and allows a pointer, and a pointer is the answer that does not make
        every caller check twice, so zero lands on the sixteen byte shelf like
        anything else under nine bytes and comes back with eight usable bytes
        and a tag free() will recognise. Two calls to malloc(0) return two
        different pointers, which is what a program that uses the pointer as
        an identity expects.
*/
pub address_any memory_take(positive bytes)
{
        return allocator_take(bytes, null);
}

/*
        free.

        Null is a no-op, and that is not a courtesy: the cleanup path of
        almost every function in a C program frees things that may never have
        been allocated, and a free that could not take null would put a test
        around every one of them.

        The tag decides the rest. A mapping goes back to the kernel whole, an
        over-aligned block hands the question to the allocation underneath it,
        and everything else goes onto the free list of the shelf it has said
        it belongs to since it was cut.

        A tag in the freed band means this block is already on a list, so the
        second free of it does nothing. A tag that is none of the above means
        the pointer did not come from here at all, or points into the middle
        of something, or the block in front of it overran and wrote over the
        word. Nothing here can tell those apart and there is no abort to reach
        for, so the block is left exactly as it is. That leaks, and leaking is
        the containment: the alternative is to index the free list array with
        whatever the number happened to be and write a pointer through it.
*/
pub fn memory_give(address_any block)
{
        if (!block)
                return;

        positive tag = address_to allocator_tag(block);

        if (tag == ALLOCATOR_SHIFTED)
        {
                memory_give((address_any)address_to allocator_extra(block));
                return;
        }

        if (tag == ALLOCATOR_MAPPED)
        {
                memory_free((address_any)((positive)block -
                                          ALLOCATOR_HEADER_WIDE),
                            address_to allocator_extra(block));
                return;
        }

        if (tag >= ALLOCATOR_CLASSES)
                return;

        address_to allocator_tag(block) = ALLOCATOR_FREED + tag;
        address_to allocator_link(block) = allocator_free_list[tag];
        allocator_free_list[tag] = block;
}

/*
        malloc_usable_size.

        How many bytes are really there, which is at least what was asked for
        and usually more, because a shelf is a rounded size. A program is
        allowed to use all of it. Reporting a shelf's whole payload rather
        than the original request is both the standard behaviour and the
        honest one -- the memory is spent either way -- and it is what lets
        realloc decide in one comparison whether a block needs to move.

        An over-aligned block reports what is left of the allocation
        underneath it after the padding that got it onto its boundary, which
        is the only figure a caller can safely write into.
*/
pub positive memory_usable_size(address_any block)
{
        if (!block)
                return 0;

        positive tag = address_to allocator_tag(block);

        if (tag == ALLOCATOR_SHIFTED)
        {
                positive inner = address_to allocator_extra(block);
                positive lead = (positive)block - inner;
                positive whole = memory_usable_size((address_any)inner);

                return whole > lead ? whole - lead : 0;
        }

        if (tag == ALLOCATOR_MAPPED)
                return address_to allocator_extra(block) - ALLOCATOR_HEADER_WIDE;

        if (tag >= ALLOCATOR_CLASSES)
                return 0;

        return allocator_class_size[tag] - ALLOCATOR_HEADER;
}

/*
        calloc.

        Two things beyond malloc. The multiplication has to be checked,
        because calloc(count, size) is the one allocation call in C that takes
        two numbers and multiplies them, and every historical hole of this
        shape has been an unchecked multiply wrapping to a small number and a
        loop then writing count elements into it. The check is a division and
        it is on the cold side of a call that is about to touch every byte of
        the result anyway.

        And the zeroing is skipped exactly when it can be proven unnecessary.
        A block cut from a chunk the kernel has only just handed over, or a
        mapping of its own, is already zero and stays zero until somebody
        writes to it; a block off a free list held somebody else's data ten
        instructions ago. allocator_take knows which of those it did and says
        so. For a large calloc that is the whole cost of the call: the pages
        are not even faulted in until they are read.
*/
pub address_any memory_take_zeroed(positive count, positive size)
{
        if (size && count > ((positive)-1) / size)
                return null;

        positive bytes = count * size;
        bool fresh = 0;
        address_any block = allocator_take(bytes, address_of fresh);

        if (!block)
                return null;

        if (!fresh && bytes)
                memory_zero(block, bytes);

        return block;
}

/*
        realloc, and its four corners.

        A null block is malloc, because that is what makes a grow-as-you-go
        loop start from nothing without a special first turn. A size of zero
        frees and answers null, which is what glibc does and what every
        program written before C23 deprecated it expects; a program that wants
        the other reading can test the size itself.

        Otherwise the question is whether the block has to move at all, and
        the answer is not "did the size go up". Sizes inside one shelf all get
        the same block, so the test is whether a fresh allocation of the new
        size would have a different usable size than this one already has. If
        it would not, the block stays exactly where it is, and a loop that
        grows a buffer a byte at a time crosses a shelf boundary about fifty
        times over the whole address space instead of copying every turn.

        When it would, the block moves, and min(old, new) bytes come with it.
        The old usable size is the right thing to copy up to rather than the
        old request, which is not written down anywhere: every byte of it is
        this block's and copying a few more of them than the caller ever wrote
        is free.

        The one case that does not move is a mapping still too large for any
        shelf. mremap resizes those in the page tables, which for anything
        over a megabyte is the difference between a syscall and a memcpy, and
        it is allowed to move it, in which case the kernel has already brought
        the contents along. If the kernel refuses -- and it can, there is no
        guarantee here -- the copy underneath catches it.

        A failed allocation while shrinking answers with the original block
        rather than null. It is still there, it is still large enough, and
        nothing was freed; answering null would be true of the new block and a
        lie about the old one, and callers write p = realloc(p, n).
*/
pub address_any memory_resize(address_any block, positive bytes)
{
        if (!block)
                return memory_take(bytes);

        if (!bytes)
        {
                memory_give(block);
                return null;
        }

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        positive usable = memory_usable_size(block);
        positive tag = address_to allocator_tag(block);

        if (allocator_fit(bytes) == usable)
                return block;

#if defined(LINUX)
        //      Linux only, because mremap is a Linux call and the macOS
        //      table beside it has no number to name. Everywhere else the
        //      copy below is the whole of realloc for a mapping, which is
        //      slower and no less correct.
        if (tag == ALLOCATOR_MAPPED &&
            bytes + ALLOCATOR_HEADER > ALLOCATOR_LARGEST)
        {
                positive have = address_to allocator_extra(block);
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive moved = (positive)system_call_4(
                        syscall(mremap),
                        (positive)block - ALLOCATOR_HEADER_WIDE, have, whole,
                        ALLOCATOR_MREMAP_MAYMOVE);

                if (moved && moved < (positive)-4095)
                {
                        address_any grown =
                                (address_any)(moved + ALLOCATOR_HEADER_WIDE);

                        address_to allocator_tag(grown) = ALLOCATOR_MAPPED;
                        address_to allocator_extra(grown) = whole;

                        return grown;
                }
        }
#endif

        address_any grown = memory_take(bytes);

        if (!grown)
                return bytes > usable ? null : block;

        memory_copy_apart(grown, block, bytes < usable ? bytes : usable);
        memory_give(block);

        return grown;
}

/*
        aligned_alloc, and the shifted block it invents.

        Sixteen and under is already true of every block this allocator hands
        out, so those requests are plain malloc and cost nothing extra. Above
        that the only way to land on a boundary is to ask for enough room to
        walk forward to one: the alignment itself, plus the sixteen bytes the
        walk has to start past so that the header written at the destination
        cannot land on the inner block's own header.

        The block that comes back is tagged shifted and remembers the inner
        pointer, and free, realloc and malloc_usable_size all follow that one
        word back to the real allocation. Which means the padding in front is
        not tracked and not reused -- it is simply part of a larger block that
        will be freed whole.

        realloc of one of these answers with an ordinary sixteen byte aligned
        block, because a resize is a new allocation and nothing in the block
        records what alignment it was originally asked to sit on. glibc does
        the same and C says nothing about the case, but a caller that keeps
        needing the boundary has to ask for it again rather than resize.

        C11 says the size passed here should be a multiple of the alignment.
        glibc does not enforce that and neither does this, because refusing
        would break the many callers that ask for a page aligned buffer of
        exactly the length they have, and because there is no case where
        honouring the request is unsafe.
*/
pub address_any memory_take_aligned(positive alignment, positive bytes)
{
        //      A power of two, and not zero. The and-with-one-less test is
        //      also true of zero, so zero is refused first.
        if (!alignment || (alignment & (alignment - 1)))
                return null;

        if (alignment <= ALLOCATOR_ALIGNMENT)
                return memory_take(bytes);

        if (alignment >= ALLOCATOR_LIMIT ||
            bytes >= ALLOCATOR_LIMIT - alignment - ALLOCATOR_HEADER_WIDE)
                return null;

        address_any inner =
                memory_take(bytes + alignment + ALLOCATOR_HEADER_WIDE);

        if (!inner)
                return null;

        positive walk = (positive)inner + ALLOCATOR_HEADER_WIDE;
        positive landed = (walk + alignment - 1) & ~(alignment - 1);
        address_any block = (address_any)landed;

        address_to allocator_tag(block) = ALLOCATOR_SHIFTED;
        address_to allocator_extra(block) = (positive)inner;

        return block;
}

/*
        posix_memalign.

        The same allocation with the older interface around it: the result
        goes through a pointer and the failure comes back as the errno value
        itself rather than being left in a global. It is stricter than
        aligned_alloc about the alignment -- a power of two AND at least the
        width of a pointer -- and that stricter rule is the standard's, not an
        opinion, so four is refused here and accepted above.

        On failure the caller's pointer is left alone rather than being set to
        null, which is what glibc does and what a caller checking the return
        value will never notice either way.
*/
pub b32 memory_take_aligned_into(address_any address_to result,
                                 positive alignment, positive bytes)
{
        if (!result)
                return ALLOCATOR_EINVAL;

        if (!alignment || (alignment & (alignment - 1)) ||
            alignment < sizeof(address_any))
                return ALLOCATOR_EINVAL;

        address_any block = memory_take_aligned(alignment, bytes);

        if (!block)
                return ALLOCATOR_ENOMEM;

        address_to result = block;
        return 0;
}

/*
        The names C knows these by.

        Aliases rather than wrappers, so malloc and memory_take are one symbol
        at one address and neither costs a jump to reach the other. A program
        may take the address of either and compare them and they will be
        equal, which is the honest answer: they are the same function and the
        prose name is the one it was written under.
*/
pub address_any malloc(positive bytes) __attribute__((alias("memory_take")));

pub fn free(address_any block) __attribute__((alias("memory_give")));

pub address_any calloc(positive count, positive size)
        __attribute__((alias("memory_take_zeroed")));

pub address_any realloc(address_any block, positive bytes)
        __attribute__((alias("memory_resize")));

pub address_any aligned_alloc(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

pub b32 posix_memalign(address_any address_to result, positive alignment,
                       positive bytes)
        __attribute__((alias("memory_take_aligned_into")));

pub positive malloc_usable_size(address_any block)
        __attribute__((alias("memory_usable_size")));

//      memalign is aligned_alloc with the arguments in the same order and
//      without C11's multiple-of rule, which this does not enforce anyway, so
//      it is the same function. Programs old enough to call it exist.
pub address_any memalign(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

#endif // !KERNEL_MODE && !WINDOWS

#endif // STANDARD_MODERN_C_STANDARD_ALLOCATOR
