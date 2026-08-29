
/*
        Compiler-owned conveniences for code that consumes library.c.

        This is an opt-in umbrella, not part of library.c's include graph.
        Including library.c alone gets the hardware routines and declarations
        only. Including this file gets those first, then the three fixed-size
        memory specializers and the source-generating compatibility macros
        below. That keeps actual C bodies out of the assembly library without
        making literal-size copies pay the general routine's dispatch cost.
*/
#ifndef STANDARD_MODERN_C_COMPILER_MEMORY
#define STANDARD_MODERN_C_COMPILER_MEMORY

#include "library.c"

/*
        Source-generating conveniences are compiler policy too.

        Neither is used by the library or by an in-tree consumer. They remain
        available to code that deliberately opts into this compatibility
        layer, while the pure library cannot expand either a C loop or a weak
        C function body.
*/
#define var_list_iter(list, count, type, action)              \
        do                                                    \
        {                                                     \
                for (int _i = 0; _i < (count); _i++)          \
                {                                             \
                        type _arg = var_list_get(list, type);  \
                        action;                               \
                }                                             \
        } while (0)

#ifdef LIBRARY_API

#define api_function(name, returned_type, default, args...) \
        WEAK pub returned_type name(args) { return default; }

#define api_type(name, type, default) \
        WEAK pub type name = default;

#else

#define api_function(name, returned_type, default, args...) \
        pub returned_type name(args)

#define api_type(name, type, default) \
        pub type name = default;

#endif // LIBRARY_API

/*
        Sizes that are known where the call is written.

        Most copies in a program are a sizeof: a structure, a fixed buffer, a
        path. The number is sitting in the source and the routine works it out
        again anyway -- reads the length, picks a width class, branches, and
        returns. At sixteen bytes that arithmetic is most of the work, and the
        work is two moves.

        So when the size is a literal the call is replaced, in place, by the
        line of moves that size needs. Three million copies on a 9950X, in
        microseconds, lower is quicker:

              bytes    routine   expanded
                  8       9714       5740
                 16      13408       5873
                 32      14727       6234
                 64      17215       7398
                128      17716      10540
                256      18278      26913   <- the routine wins again

        Which is where KNOWN_SIZE_MAX comes from. Past it the routine reaches
        for a wider path than any straight line written here can, and the call
        it costs stops mattering. Above the cutoff the macro is the call, so a
        size the compiler cannot fold and a size too large both arrive at the
        same routine, unchanged.

        Up to thirty two bytes the compiler's own expansion is already the
        best line available. It was level with hand written assembly at every
        size tried, and it has one advantage nothing here can match: it is
        emitted for whatever -march the program is built with, so a build
        allowed AVX-512 gets AVX-512 without being told. Above thirty two, in
        a build allowed nothing wider than SSE2, it lays sixteen bytes a turn,
        and hand written AVX2 is worth 1.2x at sixty four bytes and 1.4x at a
        hundred and twenty eight. That window is the only thing written by
        hand here.

        The wide bodies load everything before they store anything, because
        memory_copy is the name memmove is an alias for and the two halves are
        allowed to overlap. An expansion that interleaved would be correct on
        every test that did not overlap, which is most of them, so the check
        in src/test/exact.c slides every size through every overlap both ways.

        Which is what the other one is called after. memory_copy_apart was
        memory_copy_fast until now, and "fast" said nothing true: it is not a
        quicker memory_copy, it is memcpy where memory_copy is memmove, and
        the whole of the difference is that its two halves are promised not to
        overlap. A name that says which guarantee a caller is giving up is
        worth more than one that says the answer arrives sooner, particularly
        when the way to get it wrong is to reach for the quick-sounding one
        and hand it two regions that touch.

        Every size is checked one at a time and by its literal, because a test
        that takes its size from a loop counter reaches none of this: the
        choice is made by the compiler, from the token.
*/
#ifdef KERNEL_MODE
/*
        A kernel build compiles with -mno-sse and no vector registers at all,
        so the compiler's expansion here is general purpose moves and needs no
        permission from anybody to run. Sixty four is where that stops being
        quicker than the routine.

        Nothing in a kernel build reads cpu_has_avx2. The flag has no address
        early boot can reach, which is what head64.c found the first time
        memcpy was displaced and the machine stopped before it could say so.
*/
#define KNOWN_SIZE_MAX 64
#define KNOWN_WIDE 0
#else
#define KNOWN_SIZE_MAX 128
#if X64 && !defined(__AVX2__)
#define KNOWN_WIDE 1
#else
//      Either not this machine, or a build already allowed to emit wide moves
//      on its own, in which case the compiler's expansion is the better one.
#define KNOWN_WIDE 0
#endif
#endif

#if KNOWN_WIDE
/*
        Thirty two bytes from the front, thirty two from the back, and as many
        whole ones between as the size has room for. The back load overlaps
        the one before it whenever the size is not a multiple of thirty two,
        so the middle is written twice and no size needs a remainder.

        The destination and source are named as memory operands of exactly the
        size in hand rather than clobbering all of memory. A clobber measures
        the same in a loop that keeps nothing live across the copy, and costs
        real work in code that does.

        vzeroupper because the compiler does not know this touched the upper
        halves, and the SSE2 it emits either side of this pays a penalty on
        Intel parts for as long as they stay dirty.
*/
#define KNOWN_WIDE_ASM(body, size, back_offset, ...)                          \
        __asm__(body "   vzeroupper\n"                                        \
                : "=m"(*(p8(address_to)[size])(destination))                  \
                : [to] "r"(destination), [from] "r"(source),                  \
                  [back] "i"(back_offset),                                    \
                  "m"(*(const p8(address_to)[size])(source))                  \
                : __VA_ARGS__)

#define KNOWN_FILL_ASM(body, size, back_offset, ...)                          \
        __asm__("   vmovd %k[val], %%xmm0\n"                                  \
                "   vpbroadcastb %%xmm0, %%ymm0\n" body "   vzeroupper\n"     \
                : "=m"(*(p8(address_to)[size])(destination))                  \
                : [to] "r"(destination), [val] "r"((b32)value),               \
                  [back] "i"(back_offset)                                     \
                : __VA_ARGS__)
#endif

/*
        The size classes, chosen by a number the compiler has already folded.
        Each returns the destination, which is what the routines return.
*/
static inline INLINE address_any copy_known(address_any destination,
                                            address_any source, positive size)
{
#if KNOWN_WIDE
        if (size > 32 && cpu_has_avx2) {
                if (size <= 64)
                        KNOWN_WIDE_ASM("   vmovdqu (%[from]), %%ymm0\n"
                                       "   vmovdqu %c[back](%[from]), %%ymm1\n"
                                       "   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm1, %c[back](%[to])\n",
                                       size, size - 32, "xmm0", "xmm1");
                else if (size <= 96)
                        KNOWN_WIDE_ASM("   vmovdqu (%[from]), %%ymm0\n"
                                       "   vmovdqu 32(%[from]), %%ymm1\n"
                                       "   vmovdqu %c[back](%[from]), %%ymm2\n"
                                       "   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm1, 32(%[to])\n"
                                       "   vmovdqu %%ymm2, %c[back](%[to])\n",
                                       size, size - 32, "xmm0", "xmm1", "xmm2");
                else
                        KNOWN_WIDE_ASM("   vmovdqu (%[from]), %%ymm0\n"
                                       "   vmovdqu 32(%[from]), %%ymm1\n"
                                       "   vmovdqu 64(%[from]), %%ymm2\n"
                                       "   vmovdqu %c[back](%[from]), %%ymm3\n"
                                       "   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm1, 32(%[to])\n"
                                       "   vmovdqu %%ymm2, 64(%[to])\n"
                                       "   vmovdqu %%ymm3, %c[back](%[to])\n",
                                       size, size - 32,
                                       "xmm0", "xmm1", "xmm2", "xmm3");
                return destination;
        }
#endif
        //      Overlap safe: the compiler's memmove expansion loads every
        //      piece before it stores any of them. Checked, at -O2, from
        //      forty bytes to a hundred: three loads, then three stores.
        __builtin_memmove(destination, source, size);
        return destination;
}

static inline INLINE address_any copy_apart_known(address_any destination,
                                                 address_any source, positive size)
{
#if KNOWN_WIDE
        if (size > 32 && cpu_has_avx2)
                return copy_known(destination, source, size);
#endif
        __builtin_memcpy(destination, source, size);
        return destination;
}

static inline INLINE address_any fill_known(address_any destination,
                                            b8 value, positive size)
{
#if KNOWN_WIDE
        if (size > 32 && cpu_has_avx2) {
                if (size <= 64)
                        KNOWN_FILL_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                else if (size <= 96)
                        KNOWN_FILL_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, 32(%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                else
                        KNOWN_FILL_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, 32(%[to])\n"
                                       "   vmovdqu %%ymm0, 64(%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                return destination;
        }
#endif
        __builtin_memset(destination, value, size);
        return destination;
}


//====================================================================
//      set-literal
//====================================================================
/*
        Sets that are known where the call is written.

        strspn, strcspn and strpbrk take their members as a string, so a call
        that writes the members down has handed the compiler the whole set:
        string_span_of_set(line, " \t\n") says everything there is to say
        about which bytes end the run. The routine is told none of that. It
        walks the members every call and lays a two hundred and fifty six bit
        map on the stack, and only then starts reading the source. For a set
        of sixteen that build is longer than most of the runs it is built for,
        and for a set of sixty four it is longer than all of them.

        A literal set folds into the map instead, and the map is not the one
        the routine builds. string_span already scans a byte per value rather
        than a bit per value, for the reason written beside it: the shift and
        the mask a bit needs cost more per input byte than the two hundred and
        twenty four extra bytes of table. The routine cannot have that table
        because two hundred and fifty six stores is not a thing to do on the
        way into a scan. Read only memory can, because nobody stores anything
        at all. So the expansion is the folded table and a jump into
        string_span, which is two instructions and the scan the library
        already has.

        Three million scans on a 9950X, as a percentage of the routine, lower
        is quicker:

              members    run 0   run 4   run 16   run 64   run 256
                    1      67      41       25       46        39
                    2      66      61       26       45        38
                    3      72      65       42       68        38
                    4      49      33       32       44        37
                    8      36      28       31       42        35
                   16      10      10       16       26        33
                   64       1       2        3        7        26

        Two identical scan loops, assembled twice into one binary, differ by
        as much as a fifth of their time at a run of two hundred and fifty
        six, purely on where they land. So the long run column of that table
        is worth about that much and the short run columns, which are the ones
        the build cost shows in, are worth a great deal more.

        There is no cutoff in that table, and there is no cutoff to look for:
        the number the compiler knows here is the set and not the length of
        the run, so a win that reversed with the run could not be selected on
        anyway. The expansion has to be ahead at every run length or it cannot
        ship, and it is: the table is fewer instructions per byte than the
        bitmap probe on all three architectures -- three and a half against
        seven on x86_64, four and a half against six on arm64, six and a half
        against nine on riscv64 -- and the build is gone on top of that.

        A compare chain was tried for the small sets, which is what saves the
        table's two hundred and fifty six bytes, and it is not shippable for
        the same reason: against the table it is half the time at a run of
        nothing, level by eight bytes, and 1.8 to 4 times the time at two
        hundred and fifty six. It beats the routine everywhere and loses to
        the table everywhere it matters, and the run length is exactly what
        cannot be seen from here.

        Two sets do not want a table at all. No members is the answer without
        a scan. One member is a byte hunt, and the library's byte hunts read a
        vector at a time: strcspn against one byte is string_first_of_or_end
        and strpbrk against one byte is string_first_of, which are 2 and 3
        percent of the routine over a run of two hundred and fifty six. They
        are behind it at a run of nothing, by ten to twenty percent, because a
        vector hunt has a prologue and a set of one is the cheapest map the
        routine ever builds. That is the trade, and it is worth taking: a
        fifth at the length where nothing is found, against thirty times at
        the length where something is. They are level again by a run of one
        or two bytes. Under emulation the prologue weighs more and the
        crossing comes later -- around thirty two bytes on arm64 -- which is
        the emulator and not the machine, and is not something that could be
        measured here.

        The set has to be readable by the front end and not merely by the
        optimiser, because the table is a static initializer and a static
        initializer is folded once, early. __builtin_constant_p answers that
        question early too, so the guard below asks it about the very
        expression the table is built from rather than about the set's length
        alone. A set the front end cannot read then takes the routine, which
        is the answer, instead of a table of zeros, which would be a wrong
        answer that measured well. A named const array and a slice of one both
        fold; an array something has written into does not, and takes the
        routine.
*/

//      Whether the members hold this byte, as one folded expression, and the
//      same expression made safe to write down when it does not fold.
#define set_known_finds(members, value)                                       \
        (__builtin_memchr((const char *)(members), (int)(p8)(value),          \
                          __builtin_strlen((const char *)(members))) != 0)

#define set_known_holds(members, value)                                       \
        ((b8)(__builtin_constant_p(set_known_finds(members, value))           \
                      ? set_known_finds(members, value)                       \
                      : 0))

//      The complement, with the terminator put into the stopping set so that
//      the end of the source stops the run too. That one bit is the whole of
//      the difference between the two directions, here as in the routines.
#define set_known_stops(members, value)                                       \
        ((b8)(__builtin_constant_p(set_known_finds(members, value))           \
                      ? ((value) != 0 && !set_known_finds(members, value))    \
                      : 0))

#define set_known_length(members)                                             \
        ((positive)__builtin_strlen((const char *)(members)))

//      Tied to the guarded element above and not to the length alone, so that
//      the arm which builds a table is only taken when the bytes of that
//      table fold. Two bytes are sampled and not one: the set's own first
//      member, and the top byte, because folding is not uniform across the
//      range -- __builtin_strchr, which this was written with first, folds
//      for a byte under 128 and gives up above it. An empty set has no first
//      member to ask about and is answered by its length.
//
//      A sample and not a proof. The condition is still folded later than the
//      static initializer it guards, so a set the optimiser can read and the
//      front end cannot would still build a table of zeros; every literal in
//      src/test/exact_set.c is there to say that no such set has been found.
#define set_known(members)                                                    \
        (__builtin_constant_p(set_known_length(members)) &&                   \
         __builtin_constant_p(set_known_finds(members, 0xff)) &&              \
         (set_known_length(members) == 0 ||                                   \
          set_known_holds(members, (members)[0])))

#define SET_KNOWN_ROW_4(m, f, b)                                              \
        f(m, (b) + 0), f(m, (b) + 1), f(m, (b) + 2), f(m, (b) + 3)
#define SET_KNOWN_ROW_16(m, f, b)                                             \
        SET_KNOWN_ROW_4(m, f, (b) + 0), SET_KNOWN_ROW_4(m, f, (b) + 4),       \
        SET_KNOWN_ROW_4(m, f, (b) + 8), SET_KNOWN_ROW_4(m, f, (b) + 12)
#define SET_KNOWN_ROW_64(m, f, b)                                             \
        SET_KNOWN_ROW_16(m, f, (b) + 0), SET_KNOWN_ROW_16(m, f, (b) + 16),    \
        SET_KNOWN_ROW_16(m, f, (b) + 32), SET_KNOWN_ROW_16(m, f, (b) + 48)
#define SET_KNOWN_ROW_256(m, f)                                               \
        SET_KNOWN_ROW_64(m, f, 0), SET_KNOWN_ROW_64(m, f, 64),                \
        SET_KNOWN_ROW_64(m, f, 128), SET_KNOWN_ROW_64(m, f, 192)

/*
        The table, written out one byte value at a time because there is no
        other way to say it: the preprocessor cannot walk a string literal, so
        the two hundred and fifty six questions are asked in the source and
        the compiler answers all of them at once. It costs about six
        milliseconds of compile time and two hundred and fifty six bytes of
        read only memory per call site, and gcc folds duplicates together only
        under -fmerge-all-constants.

        A statement expression is what holds the static, and it is the one
        thing this file borrows from GNU C that the memory specializers above
        do not. A local static is the only place a table can live that is
        built from a macro argument: a file scope one cannot see the argument
        and an automatic one is thirty two vector stores on the way into every
        call, which is the whole cost back again.
*/
#define set_known_table(members, which)                                       \
        ({                                                                    \
                static const b8 _known_set[STRING_SET_BYTES] =                \
                        {SET_KNOWN_ROW_256(members, which)};                  \
                _known_set;                                                   \
        })

//      strcspn against a single byte, which is where that byte is or where
//      the string ends, as a distance.
static inline INLINE positive span_without_byte_known(string_address source,
                                                      p8 stop)
{
        return (positive)(string_first_of_or_end(source, stop) - source);
}

//      strpbrk over a folded table: where the run of bytes outside the set
//      stopped, and null rather than the terminator when it stopped because
//      the source ran out. The terminator is in the stopping table, so the
//      one byte read afterwards tells the two apart.
static inline INLINE string_address first_of_set_known(string_address source,
                                                   const b8 address_to stops)
{
        positive run = string_span(source, stops);

        return source[run] ? source + run : (string_address)null;
}

//====================================================================
//      needle-length
//====================================================================
/*
        Needle lengths that are known where the call is written.

        memory_search is a good routine and most of what it does before it
        reads anything is decide. It answers the two degenerate lengths, saves
        six registers, walks the needle to pick the two rarest bytes of it,
        broadcasts them into vectors, and only then starts on the haystack.
        Against a page of text that is the right order to do things in and the
        walk over the needle pays for itself many times over. Against a needle
        whose length is a token in the source it is arithmetic the compiler
        could have done, and for the two shortest lengths it is arithmetic
        that arrives at a routine already in the library.

        Nothing at all is the front of the haystack. That is what memmem
        answers, the routine answers it in three instructions, and folded it
        is none.

        One byte is not a substring search. It is memory_first_of, which is
        here, is wide on every machine that has vectors, and is where the
        routine itself jumps -- after nine instructions of working out that
        this is where it should jump. Folding the length removes those and the
        jump. Counted rather than argued, by instructions executed:

                                x86_64          arm64          riscv64
              haystack     routine memchr  routine memchr  routine memchr
                     4          68     57       55     44       49     39
                    16          81     70       46     35       59     49
                    64          47     36       57     46       96     86
                  4096          47     36       57     46       96     86

        Eleven instructions on two machines and ten on the third, at every
        haystack length, which on a 9950X is eighty eight to ninety one per
        cent of the routine from four bytes to sixty four kilobytes -- the
        flat shape a fixed saving off a fixed overhead has. The empty needle
        is thirteen instructions against three, and eighty six per cent.

        Two bytes and longer are deliberately NOT here. An expansion for them
        does exist and does win, and it wins only while the haystack is
        shorter than the routine's own block: thirty one bytes on x86_64,
        fifteen on arm64, and no such number at all on riscv64, which has no
        block. Measured to the byte on a 9950X against a call-free compare
        chain, the crossover is exactly where memory_search stops taking its
        narrow path, at every needle length tried -- three times quicker at
        sixteen bytes of haystack, two and a half times slower at a thousand.
        The haystack's length is not something the compiler knows, so choosing
        between those by a folded number is not possible, and choosing by a
        run-time test means a bound that differs by two to one across three
        machines and cannot be measured on two of them. A specialization whose
        gate is a guess is not one.

        memory_search_ascii_case gets the same two lengths and gets more from
        them, because its routine has no one byte redirect at all: a one byte
        needle there walks the whole candidate machinery for what
        memory_first_of_ascii_case does in one pass. Thirty four instructions
        on x86_64, fifty seven on arm64, sixty one on riscv64, and forty seven
        to sixty seven per cent of the routine timed.

        Every length is checked one at a time and by its literal, in
        src/test/needle.c, because a test that takes its needle length from a
        loop counter reaches none of this: the choice is made by the compiler,
        from the token. A search writes nothing, so guard bytes cannot show a
        read past the end of the haystack the way they show a write; the test
        lays the haystack against the last mapped byte of a page and hands the
        page after it back to the kernel instead.
*/
#define KNOWN_NEEDLE_MAX 1

static inline INLINE address_any search_known(address_any block, positive size,
                                              address_any needle,
                                              positive needle_size)
{
        if (needle_size == 0)
                return block;
        return memory_first_of(block, ((const p8 address_to)needle)[0], size);
}

static inline INLINE address_any search_case_known(address_any block, positive size,
                                                   address_any needle,
                                                   positive needle_size)
{
        if (needle_size == 0)
                return block;
        return memory_first_of_ascii_case(block,
                                          ((const p8 address_to)needle)[0], size);
}

//====================================================================
//      bounded-string
//====================================================================
//      Add to src/compiler_memory.c, immediately before the existing "A macro names itself in its own replacement" comment (so it sits with copy_known / fill_known and before the macro block). Durable copy: /private/tmp/claude-501/-Users-alve-dawning-kit/96a48472-b59c-4e34-b254-8c9750e693c6/scratchpad/block.c

/*
        Bounds that are known where the call is written.

        The bounded string routines take a number the caller nearly always
        writes down: a sizeof, the capacity of a fixed buffer, the length of a
        literal being compared against. The routine reads it, decides whether
        a wide body is worth entering, builds the two SWAR constants, and only
        then starts looking at bytes -- and under a couple of dozen bytes that
        preamble is most of the call. When the number is in the source the
        whole decision is already made, so the call is replaced by the line of
        loads the bound needs and nothing else.

        The shape is the same at every bound. Eight bytes at a time while
        eight fit, then one last eight from the back, overlapping the ones
        already walked whenever the bound is not a multiple of eight. The
        overlap needs no mask: the bytes it covers twice were found not to be
        the terminator the first time, so the first flag this word raises can
        only belong to a byte nothing has looked at. That one trick is what
        makes a bound of thirty one cost the same as a bound of thirty two
        instead of seven byte compares more, and without it the expansion
        loses at every bound that is not a multiple of eight.

        Four to seven bytes have no eight byte load in them, and there the
        same pair of overlapping loads is done four bytes wide. Below four it
        is the bytes themselves, which is three compares at most.

        Where the cutoffs come from, on a 9950X, two hundred thousand calls a
        cell, best of seven, the source longer than the bound so the walk
        never stops early -- the worst case, and the one that decides:

            bound          8    16    24    31    32    48    64
            length_max    41%   25%   38%   41%  114%   82%  207%
            compare_max   24%   29%   65%   44%   69%   73%  193%
            copy_max_end  36%   38%   39%   49%   61%   64%  105%
            endptr        31%   28%   29%   31%   68%   94%  108%
            append_max    90%   88%   88%   88%   97%   92%   99%

        and the same five with the source four bytes long instead, which is
        the other shape a bounded call has and the only one where any of them
        changes sign:

            bound          8    16    24    31    32    48    64
            length_max    37%   31%   31%   33%   49%   51%   58%
            compare_max   28%   49%   60%   49%   49%   49%   49%
            copy_max_end  26%   36%   42%   42%   46%   44%   43%
            endptr        52%   44%   46%   51%   49%   59%   54%
            append_max    89%   89%  101%  101%  105%  107%  100%

        string_append_max is the one with a third number rather than the pair's
        forty eight, and the row above is why: it is the only one of the three
        whose baseline also pays for an unbounded walk to the end of the
        destination, so what the expansion saves is a small part of the call
        and the sign of the difference is decided by things a table cannot
        see. Sixteen and under it is 88 to 98 per cent of the routine wherever
        the terminator sits; from twenty four up, with a source shorter than
        the bound, it is 100 to 109, reproducibly and on three different
        cores. So sixteen is where it stops on the machines with a wide body,
        and eight where the pair stops without one.

        string_length_max turns at thirty two because that is where its wide
        body starts. string_compare_max on x86_64 goes on winning to sixty
        three, where its AVX2 round begins -- but arm64 enters NEON at thirty
        two and turns there, and one number has to be right on both machines,
        so thirty one is where both stop. The pair that measure and then copy
        keep winning further, because what they save is a frame and two calls
        rather than a scan, and forty eight is where that stops paying.

        KERNEL_MODE needs no third number, which is worth saying because the
        expansion above it has one. A kernel build compiles ASM_USERSPACE_WIDE
        away, so the bodies these race lose their AVX-512, AVX2 and NEON
        rounds and become the word loops -- which is the shape riscv64 has all
        the time, and the reason to expect the cutoffs to move. They do not.
        Measured with cpu_has_avx2 and cpu_has_avx512 written to zero before
        the sweep, which is that shape exactly, on the same box and the same
        harness:

            bound          8    16    24    31    32    48    56
            length_max    40%   27%   43%   53%  117%    -     -
            compare_max   24%   33%   68%   45%  100%    -     -
            copy_max_end  43%   35%   45%   52%   50%   55%  100%
            endptr        28%   33%   38%   39%   52%   55%  100%

        Thirty one and forty eight, again, from both directions. What the
        expansion loses in a kernel build it loses to a routine that got
        slower too, and the two move together.

        riscv64 is the exception and it is not a small one. It has no vector
        at the baseline, so its bounded bodies already are the word loop this
        would expand to, and an unaligned eight byte load is not part of the
        RV64I floor -- which is exactly why the library's own riscv bodies
        align down before they read a word. So the word step is not written
        there at all, and what is left is the byte walk, which is only ahead
        while the bound is very short. Guest instructions a call, counted with
        kit/insn.c, source longer than the bound:

            bound             1     2     4     8    16
            length_max     30/10 30/13 30/29 30/49 39/89
            copy_max_end   79/14 86/17 77/40 77/78 88/120

        Four, and eight for the pair, is where those cross.

        string_copy_max is the one that is not here, and the table is why. It
        is already a wide scan that tail calls the wide copy, so all an
        expansion can take away is the call itself, and the byte walk it puts
        in its place is behind from six bytes on arm64 and from eight on
        x86_64 -- 22% of the routine at a bound of one, 71% at four, 107% at
        six, 241% at sixteen. The window that wins is one to four, and the
        literal bounds handed to it in the tree are nought, five and ten. A
        guard nothing trips is not worth three lines, so it was measured and
        left out.

        Every one of these is correct at any bound, not just below its own
        cutoff -- they are slower above it, never wrong -- which is not true
        of copy_known above, and is worth knowing before one of them is
        called by hand.
*/
#if RISCV64
#define KNOWN_BOUND_MAX 4
#define KNOWN_PAIR_MAX 8
#define KNOWN_APPEND_MAX 8
#else
#define KNOWN_BOUND_MAX 31
#define KNOWN_PAIR_MAX 48
#define KNOWN_APPEND_MAX 16
#endif

#if X64 || ARM64
#define KNOWN_BOUND_WORDS 1
#else
#define KNOWN_BOUND_WORDS 0
#endif

#define KNOWN_BOUND_ONES 0x0101010101010101ull
#define KNOWN_BOUND_HIGH 0x8080808080808080ull
#define KNOWN_NARROW_ONES 0x01010101u
#define KNOWN_NARROW_HIGH 0x80808080u

#if KNOWN_BOUND_WORDS
/*
        A word from wherever the pointer is, and which of its bytes is the
        terminator. Both loads are unaligned and both are inside the bound,
        which is the same licence the routines take: the caller promising a
        bound is the caller promising that many bytes are there to read.

        Only x86_64 and arm64 get here, so the count of trailing zeros is one
        instruction and there is no libgcc call hiding in it.
*/
static inline INLINE p64 bound_word_at(string_address where)
{
        p64 word;

        __builtin_memcpy(&word, where, sizeof(word));

        return word;
}

static inline INLINE p32 bound_narrow_at(string_address where)
{
        p32 word;

        __builtin_memcpy(&word, where, sizeof(word));

        return word;
}

#define KNOWN_ENDS(word) (((word) - KNOWN_BOUND_ONES) & ~(word) & KNOWN_BOUND_HIGH)
#define KNOWN_NARROW_ENDS(word) \
        (((word) - KNOWN_NARROW_ONES) & ~(word) & KNOWN_NARROW_HIGH)

static inline INLINE positive bound_flag_byte(p64 flags)
{
        return (positive)__builtin_ctzll(flags) >> 3;
}

static inline INLINE positive bound_narrow_byte(p32 flags)
{
        return (positive)__builtin_ctz(flags) >> 3;
}
#endif

//      strnlen: how far in the terminator is, or the bound when it is not
//      inside it at all. Nothing here reads past source + bound.
static inline INLINE positive length_max_known(string_address source, positive bound)
{
        positive walked = 0;

#if KNOWN_BOUND_WORDS
        if (bound >= 8) {
                while (walked + 8 <= bound) {
                        p64 word = bound_word_at(source + walked);

                        if (KNOWN_ENDS(word))
                                return walked + bound_flag_byte(KNOWN_ENDS(word));

                        walked += 8;
                }

                //      The last eight, overlapping. What the overlap covers
                //      twice cannot be the terminator, so nothing is masked.
                if (walked < bound) {
                        p64 word = bound_word_at(source + bound - 8);

                        if (KNOWN_ENDS(word))
                                return bound - 8 +
                                       bound_flag_byte(KNOWN_ENDS(word));
                }

                return bound;
        }

        if (bound >= 4) {
                p32 word = bound_narrow_at(source);

                if (KNOWN_NARROW_ENDS(word))
                        return bound_narrow_byte(KNOWN_NARROW_ENDS(word));

                if (bound > 4) {
                        word = bound_narrow_at(source + bound - 4);

                        if (KNOWN_NARROW_ENDS(word))
                                return bound - 4 +
                                       bound_narrow_byte(KNOWN_NARROW_ENDS(word));
                }

                return bound;
        }
#endif

        for (; walked < bound; walked++)
                if (!source[walked])
                        return walked;

        return bound;
}

/*
        strncmp, and the answer is the magnitude of the difference and not
        merely its sign, which is what src/test/verify.c asserts and what the
        assembly returns. So no builtin can stand in for this: __builtin_strncmp
        at a constant length emits a call to strncmp under -fno-builtin, and
        the sign-only contract would be the wrong one even if it did not.

        A word that differs, or a word holding the terminator, drops into the
        byte walk, which is the only code here that produces a value. The
        overlapping word at the end is only consulted when no earlier word
        differed, because a difference behind it would otherwise be answered
        as equality.
*/
static inline INLINE b32 compare_max_known(string_address source, string_address input,
                                           positive bound)
{
        positive walked = 0;

#if KNOWN_BOUND_WORDS
        bool differed = 0;

        while (walked + 8 <= bound) {
                p64 left = bound_word_at(source + walked);
                p64 right = bound_word_at(input + walked);

                if (left != right) {
                        differed = 1;
                        break;
                }

                if (KNOWN_ENDS(left))
                        return 0;

                walked += 8;
        }

        if (!differed && bound >= 8 && walked < bound) {
                p64 left = bound_word_at(source + bound - 8);
                p64 right = bound_word_at(input + bound - 8);

                if (left == right && !KNOWN_ENDS(left))
                        return 0;
        }
#endif

        for (; walked < bound; walked++) {
                if (source[walked] != input[walked])
                        return (b32)source[walked] - (b32)input[walked];

                if (!source[walked])
                        return 0;
        }

        return 0;
}

/*
        The bounded copy that terminates, and where it ended. The buffer holds
        bound + 1 bytes, which is the routine's precondition and stays this
        one's.

        Under eight bytes there is no word to load and the call to the copy
        costs more than the bytes it would move, so the whole of it is written
        out. From eight up the measure is expanded and the move is left to
        memory_copy_apart, which is wide and is already the right answer -- and
        the measure is used past its own cutoff on purpose here, because what
        this is racing is a frame and two calls rather than a scan.
*/
static inline INLINE p8 address_to copy_max_end_known(p8 address_to into,
                                                      string_address source,
                                                      positive bound)
{
        positive length;

        if (bound < 8) {
                length = 0;

                while (length < bound && source[length]) {
                        into[length] = source[length];
                        length++;
                }

                into[length] = 0;

                return into + length;
        }

        length = length_max_known(source, bound);

        memory_copy_apart(into, source, length);
        into[length] = 0;

        return into + length;
}

/*
        stpncpy: the same measure, and then the rest of the bound zeroed. A
        source that fills the bound leaves no terminator, which is the rule
        that surprises people and is why the padding is the only conditional
        part -- the answer is destination plus the length either way.
*/
static inline INLINE p8 address_to copy_max_endptr_known(string_address destination,
                                                         string_address source,
                                                         positive bound)
{
        positive length;

        if (bound < 8) {
                length = 0;

                while (length < bound && source[length]) {
                        destination[length] = source[length];
                        length++;
                }

                for (positive pad = length; pad < bound; pad++)
                        destination[pad] = 0;

                return destination + length;
        }

        length = length_max_known(source, bound);

        memory_copy_apart(destination, source, length);

        if (length < bound)
                memory_fill(destination + length, 0, bound - length);

        return destination + length;
}

/*
        strncat, which is the walk to the end of the destination and then the
        bounded copy that terminates. The bound counts source bytes only and
        the walk it is not attached to is unbounded, so a literal bound cannot
        make that half any shorter -- what it removes is the routine's own
        frame and the second call. On x86_64 that is level, and on arm64 and
        riscv64, where the frame is three saved registers and a link, it is
        worth a fifth of the call.
*/
static inline INLINE string_address append_max_known(string_address destination,
                                                     string_address source,
                                                     positive bound)
{
        copy_max_end_known(destination + string_length(destination), source, bound);

        return destination;
}

//====================================================================
//      compare-and-find
//====================================================================
//      This is the complete block to add to src/compiler_memory.c. It goes AFTER fill_known and BEFORE the "A macro names itself in its own replacement" comment -- the cutoff #define block must go with it and must sit before the bodies, exactly as KNOWN_SIZE_MAX does for the copy. The byte-exact file is at /private/tmp/claude-501/-Users-alve-dawning-kit/96a48472-b59c-4e34-b254-8c9750e693c6/scratchpad/known_scan.inc and the whole spliced compiler_memory.c is at .../scratchpad/compiler_memory.candidate.c.

/*
        Lengths that are known where the call is written.

        The three above write; these three read, and the argument is the same
        one. A memcmp of sixteen bytes is two loads and a compare, and the
        routine that answers it reads the length, decides whether the length
        is worth a vector, decides whether it is worth a word, and returns --
        which at sixteen bytes is most of the instructions executed. A memchr
        of eight is one load and the zero-byte trick, and the routine spends
        about twenty instructions smearing the byte across a word and aligning
        the pointer down before it looks at anything.

        So when the length is a literal the call is replaced by the line of
        loads that length needs.

        What is different from the copy is that the answer's position matters
        as much as the length. A copy of a hundred bytes always copies a
        hundred bytes; a compare of a hundred that differ in the first one
        stops there, and the routine's whole prologue is then the cost. So
        every length below was timed three ways -- the answer at the front,
        the answer at the back, and no answer at all -- and the cutoff is
        where the routine wins in the worst of the three. Two hundred thousand
        calls on a 9950X, rotating across eight blocks so nothing is hoisted,
        in ticks, lower is quicker:

              bytes    routine  expanded     routine  expanded
                       --- memcmp, equal ---  --- memchr, no match ---
                  1    1789359    636916       2436982    428194
                  4    3043711    980701       2586321   1214062
                  8    2342726    979024       2695799    977003
                 16    2891750    637217       3902379    973950
                 31    5372677   1447638       5575336   2225078
                 32    2650047    942173       1809268   2079437  <- vectors
                 48    2771951   1177770       3911194   2771608
                 64    3270021   1435426       1826167   3557777
                 96    3665277   1922573       2260682   5087631
                127    3808811   3067534       2176617   6511929
                128    2356228   2502944       2176617   6564982

        The two hunts turn over at thirty two, which is exactly where the
        routine reaches for AVX2: below it the routine is a word walk behind
        a call and a long setup and the expansion beats it at every length
        and every position, and once the vector arrives no straight line of
        integer loads catches up again.

        memcmp keeps going all the way to a hundred and twenty seven, and
        turns over at a hundred and twenty eight, which is again exactly
        where the routine changes shape -- that is the length at which it
        starts reading four blocks a round instead of one. Below that its
        vector path is one block at a time with a call and a flag test in
        front of it, and sixteen pairs of loads written out beat it in all
        three positions.

        The other two machines put their thresholds in the same place and the
        cutoffs follow them. arm64 reaches for ldp q0, q1 at thirty two in
        memcmp and for a NEON block at sixteen in both hunts, and below those
        it runs the same word walk the expansion is, so what the expansion
        saves there is the call and nothing else -- and unlike x86_64 there
        is no runtime flag to test, so there is no reason to expect it to
        keep winning above the threshold and it is not claimed to.

        riscv64 has no vector unit in these bodies and no promise that an
        unaligned ld completes either, so the expansion there is a byte walk
        that saves the call and the setup and not the width. It wins to
        sixteen on memcmp and to twelve on the hunts, and loses above that to
        the routine's own eight-at-a-time loop. It is also the one that could
        cost room -- sixteen bytes of memcmp written out a byte at a time is
        about sixty instructions where the call is five -- but that is a
        worry about a call site nothing in the tree has: building
        src/test/standard.c with all three of these against without them
        moves .text by 128 bytes down on x86_64, 32 down on arm64 and 32 up
        on riscv64. A build that wants size more than speed should still
        lower the riscv numbers before the others.

        The numbers for both of those are qemu's and are worth their
        direction and not their ratio, which is why what is proposed for them
        is the threshold their own assembly already names.

        A kernel build has neither AVX2 nor NEON in these bodies, so the
        routine is weaker there and every number here is a conservative one.

        Every length is checked one at a time and by its literal, because a
        test that takes its length from a loop counter reaches none of this:
        the choice is made by the compiler, from the token.
*/
//      Keyed the same way round as KNOWN_SCAN_WORDS below, so that a machine
//      which is none of the three named ones gets the byte walk and the byte
//      walk's cutoffs rather than one of each.
#if X64
#define KNOWN_COMPARE_MAX 127
#define KNOWN_SCAN_MAX 31
#elif ARM64
#define KNOWN_COMPARE_MAX 31
#define KNOWN_SCAN_MAX 15
#else
#define KNOWN_COMPARE_MAX 16
#define KNOWN_SCAN_MAX 12
#endif

/*
        These read only inside the caller's bound. Every load lies within
        [block, block + size), the last word of a length that is not a
        multiple of eight overlapping the one before it rather than reaching
        past the end, which is the same shape the copy uses at its tail and
        for the same reason: no length needs a remainder, and nothing is read
        that the caller did not hand over. src/test/exact_scan.c lays each
        block against an unmapped page, once at each end, so that a read of
        one byte too far stops the program rather than passing.
*/

/*
        Every loop below has a trip count the compiler already knows, and a
        straight line is what this whole file is for -- but gcc stops fully
        unrolling somewhere between two turns and eight, and a rolled loop at
        forty bytes is a counter and a backward branch per eight bytes on top
        of the work. Saying so is worth 1.4x on memcmp from thirty two bytes
        up on x86_64, and it is what moves its cutoff from forty eight to a
        hundred and twenty seven; on riscv64, where the walk is a byte at a
        time, it is worth 1.5x at eight bytes and more above it.

        Every caller that exists today is one of the three macros at the
        bottom of this file, and each of those checks that the length is
        folded before it expands, so what is unrolled here is always a loop
        whose trip count is a number. A caller that reached one of these
        directly with a length worked out at run time would still get a
        correct answer -- the pragma leaves a remainder loop behind it -- and
        would only pay for the room.
*/
#define KNOWN_STRAIGHT _Pragma("GCC unroll 16")

/*
        Whether an integer load may sit at any address.

        x86_64 and arm64 both complete an unaligned load in one instruction,
        so a word walk over a caller's pointer is free. Baseline RV64I makes
        no such promise, and every riscv body in library.c is written around
        that -- memory_first_of aligns down and masks, memory_last_of peels,
        memory_compare checks that the two pointers share a residue and byte
        walks when they do not. An expansion is not allowed to be laxer than
        the routine it replaces, so on riscv these stay byte at a time, where
        what they save is the call and the setup and not the width.
*/
#if X64 || ARM64
#define KNOWN_SCAN_WORDS 1
#else
#define KNOWN_SCAN_WORDS 0
#endif

#if KNOWN_SCAN_WORDS
//      The two constants the carry-free byte tests are built from.
#define KNOWN_ONES 0x0101010101010101ull
#define KNOWN_HIGHS 0x8080808080808080ull

/*
        A word from anywhere, and the byte order that reads it.

        __builtin_memcpy into a local is the spelling that does not claim the
        address is aligned; on both machines here it is the one load it looks
        like, and the width is a literal at every call below so nothing here
        is decided at run time. The top of the word is left zero, so a four
        or two byte read compares and differences exactly as an eight byte
        one does.

        Byte i of the block is byte i of the word because both machines are
        little endian, which the whole tree already assumes, so an earlier
        byte is a lower shift and "first" is "lowest".
*/
static inline INLINE p64 known_word(const p8 address_to at, positive width)
{
        p64 word = 0;

        __builtin_memcpy(address_of word, at, width);
        return word;
}

/*
        Where the lowest nonzero byte of a word begins, as a shift, and where
        the highest one does.

        Neither builtin says anything about a word of zero, so neither is
        reached with one: every caller below has already asked whether the
        two words differed, or whether any byte matched, and is inside that
        branch.
*/
static inline INLINE positive known_lowest_byte(p64 word)
{
        return (positive)__builtin_ctzll(word) & ~(positive)7;
}

//      The other end, for the search that runs backwards.
static inline INLINE positive known_highest_byte(p64 word)
{
        return ((positive)63 - (positive)__builtin_clzll(word)) & ~(positive)7;
}

/*
        The difference memcmp promises, from two words already in registers.

        Exclusive-or leaves zero bytes where the two agree, so the first byte
        that differs is the one holding the lowest set bit. The answer is the
        difference of those two bytes read as unsigned char, which is a
        magnitude and not merely a sign -- src/test/verify.c asserts the
        number, and the assembly returns it, so an expansion that returned
        -1, 0 or 1 would be quicker and wrong.
*/
static inline INLINE b32 known_word_difference(p64 first_word, p64 second_word)
{
        positive at = known_lowest_byte(first_word ^ second_word);

        return (b32)((first_word >> at) & 0xff) -
               (b32)((second_word >> at) & 0xff);
}

/*
        Which bytes of a word are zero, marked in their high bit.

        The cheap form, (word - ones) & ~word & highs, marks the first zero
        byte correctly and may also mark a byte holding one just above it,
        because the borrow that the zero byte sent out is indistinguishable
        from the one that byte would have sent. Whoever wants the lowest mark
        may use it and the library does. A search that runs backwards wants
        the highest mark and may not, so this is the exact form: a byte
        survives only when every bit of it was clear.
*/
static inline INLINE p64 known_zero_bytes(p64 word)
{
        p64 low = word & ~KNOWN_HIGHS;

        return ~((low + ~KNOWN_HIGHS) | word | ~KNOWN_HIGHS);
}
#endif // KNOWN_SCAN_WORDS

/*
        memcmp at a length the compiler folded.

        Whole words while there is room for one, then the last eight bytes
        read again, which is inside the bound whenever the size reached eight
        at all and costs nothing: the bytes it looks at twice are bytes the
        word before it already found equal, so the first difference it can
        report is still the first difference there is.

        Correct at every size, not only under the cutoff. The macro below
        never calls this with a large one, but a specializer that is only
        right in the window it is used in is a trap for whoever writes the
        next one.
*/
static inline INLINE b32 compare_known(const address_any first,
                                       const address_any second, positive size)
{
        const p8 address_to left = (const p8 address_to)first;
        const p8 address_to right = (const p8 address_to)second;
        positive at = 0;

#if KNOWN_SCAN_WORDS
        if (size >= 8) {
                KNOWN_STRAIGHT
                while (at + 8 <= size) {
                        p64 one = known_word(left + at, 8);
                        p64 two = known_word(right + at, 8);

                        if (one != two)
                                return known_word_difference(one, two);
                        at += 8;
                }

                if (at != size) {
                        p64 one = known_word(left + size - 8, 8);
                        p64 two = known_word(right + size - 8, 8);

                        if (one != two)
                                return known_word_difference(one, two);
                }

                return 0;
        }

        //      Under eight, the same shape in whatever width fits twice: a
        //      four byte compare is one of the commonest literals there is
        //      and deserves better than four branches. The second load of
        //      each pair overlaps the first, so five, six and seven need no
        //      remainder either.
        if (size >= 4) {
                p64 one = known_word(left, 4);
                p64 two = known_word(right, 4);

                if (one != two)
                        return known_word_difference(one, two);
                if (size == 4)
                        return 0;
                one = known_word(left + size - 4, 4);
                two = known_word(right + size - 4, 4);
                if (one != two)
                        return known_word_difference(one, two);
                return 0;
        }

        if (size >= 2) {
                p64 one = known_word(left, 2);
                p64 two = known_word(right, 2);

                if (one != two)
                        return known_word_difference(one, two);
                if (size == 2)
                        return 0;
                one = known_word(left + size - 2, 2);
                two = known_word(right + size - 2, 2);
                if (one != two)
                        return known_word_difference(one, two);
                return 0;
        }
#endif

        KNOWN_STRAIGHT
        for (; at < size; at++)
                if (left[at] != right[at])
                        return (b32)left[at] - (b32)right[at];

        return 0;
}

/*
        memchr at a length the compiler folded.

        The byte smeared across a word, exclusive-ored in so a match becomes
        a zero byte, and the carry-free test that finds one. The lowest mark
        is the first match, which is the direction the cheap test is exact
        in. Under eight bytes the setup costs more than the compares it would
        save, so that tier is the compares.
*/
static inline INLINE address_any first_of_known(address_any block, b8 value,
                                                positive size)
{
        p8 address_to at = (p8 address_to)block;
        p8 wanted = (p8)value;
        positive done = 0;

#if KNOWN_SCAN_WORDS
        if (size >= 8) {
                p64 spread = (p64)wanted * KNOWN_ONES;

                KNOWN_STRAIGHT
                while (done + 8 <= size) {
                        p64 mixed = known_word(at + done, 8) ^ spread;
                        p64 hit = (mixed - KNOWN_ONES) & ~mixed & KNOWN_HIGHS;

                        if (hit)
                                return at + done + (known_lowest_byte(hit) >> 3);
                        done += 8;
                }

                if (done != size) {
                        p64 mixed = known_word(at + size - 8, 8) ^ spread;
                        p64 hit = (mixed - KNOWN_ONES) & ~mixed & KNOWN_HIGHS;

                        //      Anything in the bytes this reads twice was
                        //      looked at by the word before and did not
                        //      match, so the lowest mark here is still the
                        //      first match in the block.
                        if (hit)
                                return at + size - 8 +
                                       (known_lowest_byte(hit) >> 3);
                }

                return null;
        }
#endif

        KNOWN_STRAIGHT
        for (; done < size; done++)
                if (at[done] == wanted)
                        return at + done;

        return null;
}

/*
        memrchr at a length the compiler folded.

        The same walk from the other end, and the exact zero-byte mask rather
        than the cheap one, because the highest mark is the answer here and
        the cheap test is only exact about its lowest.
*/
static inline INLINE address_any last_of_known(address_any block, b8 value,
                                               positive size)
{
        p8 address_to at = (p8 address_to)block;
        p8 wanted = (p8)value;
        positive left = size;

#if KNOWN_SCAN_WORDS
        if (size >= 8) {
                p64 spread = (p64)wanted * KNOWN_ONES;

                KNOWN_STRAIGHT
                while (left >= 8) {
                        p64 hit = known_zero_bytes(known_word(at + left - 8, 8) ^
                                                   spread);

                        if (hit)
                                return at + left - 8 +
                                       (known_highest_byte(hit) >> 3);
                        left -= 8;
                }

                if (left) {
                        p64 hit = known_zero_bytes(known_word(at, 8) ^ spread);

                        //      The bytes above the remainder were looked at
                        //      by the word before and did not match, so the
                        //      highest mark here is still the last match in
                        //      the block.
                        if (hit)
                                return at + (known_highest_byte(hit) >> 3);
                }

                return null;
        }
#endif

        KNOWN_STRAIGHT
        while (left) {
                left--;
                if (at[left] == wanted)
                        return at + left;
        }

        return null;
}

//====================================================================
//      number-base
//====================================================================
/*
        Bases that are known where the call is written.

        A base is a literal at nearly every call site there is. Ten is what a
        program means when it reads a port or a count or a line number, and
        sixteen is what it means when it reads an address or a colour. The
        routine cannot know that: it takes the base in a register, checks it
        is one of the thirty five it accepts, asks whether it is zero and so a
        prefix to be detected, and then carries it through the digit loop as a
        value it compares against once per byte.

        Every one of those questions has an answer at compile time when the
        base is a token, and the answers change the shape of the work rather
        than shortening it. A base below eleven has no letters in it at all,
        so the two alphabet ranges, the branch between them and the base
        compare all go and one subtract and one unsigned compare classify a
        byte outright. A power of two accumulates by shift, and its overflow
        is the bits the shift is about to lose rather than the top half of a
        multiply. positive_into_base with base ten is positive_into, which is
        where it jumps anyway after eleven instructions of deciding so. A
        base that is neither divides by a constant, which is a multiply and a
        shift where the routine has a div.

        A million parses on a 9950X, in microseconds, lower is quicker, and
        the digit count is what the row is:

              digits      base 10          base 16          base 8
                       routine  ours    routine  ours    routine  ours
                   1      9057  5134       9448  8414      9197  5225
                   2     10813  7917      11283 10866     11091  8301
                   3     12966 10068      12926 13214     13196  9739
                   4     15000 12371      15422 15099     14488 11861
                   6     18184 16961      20370 20171     18614 14955
                   8     22501 22093      25049 23893     22506 18557
                  12     31175 29873      37875 30681     31142 24622
                  16     40921 38511      50324 37697     40801 30208

        Base ten is 56% of the routine at one digit and 73% at two, which is
        where a call site actually lives, and closes to level around eight,
        where both sides are waiting on the same multiply chain and there is
        nothing left to remove. Base eight and base two are the same shape and
        better, 56% to 82%. Sixteen keeps its letters but accumulates by
        shift rather than by multiply: level to six digits, one to two points
        behind at three, and 74% at sixteen digits where the shift has
        replaced the whole chain.

        There is no length at which the routine wins back a base below eleven
        or sixteen, so unlike the size specializers above this one has no
        cutoff -- only a list of which bases it is for. The bases in between
        and above keep both the letters and the multiply and have nothing left
        to fold: base twelve measured 100% to 105% from four digits up and
        base thirty six 100% to 109%, so they are the ordinary call, and so is
        base zero, which asks for the prefix to be detected rather than
        folded.

        Under qemu, which is emulator work and not a machine, the same rows
        run 42% to 89% on arm64 and 18% to 66% on riscv64: an emulated tick is
        closer to an instruction than to a cycle, so those numbers say the
        instruction count fell and say nothing about what a real core would do
        with it.

        An eight-digit-at-a-time SWAR chunk was written and timed and is not
        here. It breaks the multiply chain and is worth a great deal once the
        number is long -- 60% of the routine at eight digits, 46% at nineteen
        -- but it pays a whole word load and two masked compares that fail on
        every number shorter than eight, which puts one to six digits back at
        80% to 103% where the line above has them at 56% to 93%. Call sites
        parse short numbers, so the shorter expansion is the one that ships.

        The other fold the shape invites, an end pointer known to be null,
        does not want a path of its own: the store is written as a branch on a
        pointer the compiler has folded, so a null one removes the test and
        the store by itself and a real one keeps both. It is worth about six
        points at one and two digits -- 51% and 63% against 57% and 73% -- and
        nothing at all past four, and it costs no source at all.
*/

//      One digit for a base that is folded. Anything that is not a digit of
//      that base answers the base itself, which no digit of it can be.
static inline INLINE positive digit_known(p8 character, positive base)
{
        p32 narrow = (p32)character - 48;

        if (base <= 10)
                return narrow < (p32)base ? narrow : base;
        if (narrow <= 9)
                return narrow;

        narrow = (p32)(character | 32) - 97;
        return narrow <= (p32)base - 11 ? narrow + 10 : base;
}

/*
        strtol and strtoul with the base folded.

        The signed and unsigned contracts differ only in what an overflow
        answers and whether there is a clamp, so one body carries both and the
        two macros below fold the flag away with the base. Everything the
        routine promises is promised here: leading space, an optional sign, a
        hexadecimal prefix stepped over only when a hexadecimal digit follows
        it, a saturating answer, and an end pointer that is the input itself
        when nothing was converted.

        Correct for a base of two through thirty six and for nothing else, the
        way copy_known above is correct up to KNOWN_SIZE_MAX and no further.
        Base zero, which asks the routine to read a prefix and decide, answers
        zero for every input here, because the first digit is classified
        against a base no digit can be below. The macro at the foot of this
        file is the whole of the guard, and it is a quiet failure rather than
        a loud one, so a caller that reaches past it gets a wrong answer and
        no diagnostic.
*/
static inline INLINE positive number_known(string_address input,
                                           string_address address_to stopped,
                                           positive base, bool is_signed)
{
        string_address at = input;
        positive value = 0;
        positive overflowed = 0;
        positive negative = 0;
        positive digit;
        p8 character;

        //      Tab through carriage return is one range and the space is the
        //      other, which is byte_is_space unrolled into the scan the way
        //      the routine unrolls it.
        for (;;)
        {
                character = address_to at;
                if (character != 32 && (p32)(character - 9) > 4)
                        break;
                at++;
        }

        if (character == 45)
        {
                negative = 1;
                at++;
        }
        else if (character == 43)
                at++;

        //      The prefix is stepped over only when a hexadecimal digit
        //      follows it. Left on the zero otherwise, the loop below takes
        //      that zero and stops on the x by itself, which is the answer C
        //      asks for with no case written for it. Nothing reads past a
        //      terminator: the second byte is only looked at once the first
        //      has been found to be a zero.
        if (base == 16 && address_to at == 48)
        {
                if ((p8)(at[1] | 32) == 120 && digit_known(at[2], 16) < 16)
                        at += 2;
        }

        //      The first digit is peeled because it is the one that decides
        //      whether a conversion happened at all.
        digit = digit_known(address_to at, base);
        if (digit >= base)
        {
                if (stopped)
                        address_to stopped = input;
                return 0;
        }

        value = digit;
        at++;

        for (;;)
        {
                //      One subtract and one unsigned compare classify both
                //      ends at once, and the byte is held at the width the
                //      load already zero extended it to, so neither the digit
                //      nor the letter needs a widening step to reach the
                //      accumulator.
                p32 raw = address_to at;
                p32 narrow = raw - 48;

                digit = narrow;
                if (base <= 10)
                {
                        if (narrow >= (p32)base)
                                break;
                }
                else if (narrow > 9)
                {
                        narrow = (raw | 32) - 97;
                        if (narrow > (p32)base - 11)
                                break;
                        digit = narrow + 10;
                }

                if ((base & (base - 1)) == 0)
                {
                        //      A power of two accumulates by shift, and its
                        //      overflow is exactly the bits the shift is
                        //      about to lose.
                        positive width = base == 2    ? 1
                                         : base == 4  ? 2
                                         : base == 8  ? 3
                                         : base == 16 ? 4
                                                      : 5;

                        overflowed |= value >> (64 - width);
                        value = (value << width) | digit;
                }
                else
                {
                        //      The overflow is the half of the multiply a
                        //      sixty four bit result throws away, and the
                        //      carry out of the digit folds into it, which is
                        //      one multiply and one add-with-carry rather
                        //      than a division to find a cutoff with.
                        p128 product = (p128)value * base + digit;

                        value = (positive)product;
                        overflowed |= (positive)(product >> 64);
                }

                at++;
        }

        if (stopped)
                address_to stopped = at;

        if (!is_signed)
        {
                //      An unsigned overflow answers all ones and the sign is
                //      not applied to it, which is what strtoul does; one
                //      that did not overflow negates in the machine word, so
                //      "-1" is the largest unsigned number and not an error.
                if (overflowed)
                        return ~(positive)0;
                return negative ? -value : value;
        }

        //      The signed limit is one larger on the minus side, and negating
        //      that limit is exactly the most negative number, so the same
        //      two steps serve both ends.
        positive limit = (positive)0x7fffffffffffffffUL + negative;

        if (overflowed || value > limit)
                value = limit;
        return negative ? -value : value;
}

/*
        positive_into_base with the base folded.

        Base ten is the decimal engine, reached without the range check, the
        power-of-two test and the four base compares that stand in front of
        it. A power of two writes its field from the right edge with the mask,
        the shift and the alphabet all folded into the line rather than chosen
        at run time. Any other base divides by a constant, which gcc turns
        into a multiply and a shift where the routine has one div per digit,
        and that is where most of this is:

            digits         1     2     4     8    12    16    19
            base 10       88%   75%   82%   92%   96%   99%   97%
            base 16       74%   69%   79%   87%   91%   93%   93%
            base 8       122%  100%   89%   89%   94%   94%   95%
            base 2        66%   74%   80%   83%   88%   91%   92%
            base 3        70%   66%   61%   61%   64%   62%   63%
            base 36       47%   59%   57%   53%   50%   54%   59%

        The one row above a hundred is a single octal digit, and it is 5.6
        microseconds against 6.8 on a million conversions -- about one tick
        each, on the shortest field this routine can be asked for, against a
        routine whose entire path there is perfectly predicted. Every base
        expands anyway: the alternative is a list with one hole in it for a
        case that costs a tick, and two octal digits are already level and
        four are 89%.

        Under qemu the same rows are 68% to 89% on arm64 and 23% to 86% on
        riscv64, which is instruction count and not time.

        Correct for two through thirty six and for nothing else, and quietly
        wrong outside it in two different ways: base one passes the
        power-of-two test, because one and zero have no bits in common, and
        falls through the width ladder to five; base thirty seven indexes one
        past the end of its half of the alphabet. The macro at the foot of
        this file is the whole of the guard, and the routine it replaces
        answers zero for both of those.
*/
//      The alphabet the letters come from. A folded base below eleven never
//      reaches it, and the compiler drops it from a program that has no call
//      above ten.
static const p8 base_alphabet_known[] =
        "0123456789abcdefghijklmnopqrstuvwxyz"
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/*
        The index of the highest set bit, which is what bsr answers.

        x86_64 has bsr and arm64 has clz, and gcc emits either of them from
        the builtin. A riscv baseline has neither -- count-leading-zeros lives
        in Zbb, which is deliberately not in it -- and the builtin there emits
        a call to a libgcc helper that a -nostdlib link cannot resolve, so
        that architecture gets the same halving search its own assembly uses.
        Zero has no highest bit and never arrives: the caller answers it with
        the one byte digit before asking.
*/
static inline INLINE p32 top_bit_known(positive value)
{
#if X64 || ARM64
        return 63 - (p32)__builtin_clzll(value);
#else
        p32 top = 0;

        if (value >> 32)
        {
                value >>= 32;
                top += 32;
        }
        if (value >> 16)
        {
                value >>= 16;
                top += 16;
        }
        if (value >> 8)
        {
                value >>= 8;
                top += 8;
        }
        if (value >> 4)
        {
                value >>= 4;
                top += 4;
        }
        if (value >> 2)
        {
                value >>= 2;
                top += 2;
        }
        return top + (p32)(value >> 1);
#endif
}

static inline INLINE positive into_base_known(p8 address_to into,
                                              positive value, positive base,
                                              bool upper)
{
        if (base == 10)
                return positive_into(into, value);

        if (value == 0)
        {
                address_to into = 48;
                return 1;
        }

        const p8 address_to alphabet = base_alphabet_known + (upper ? 36 : 0);

        if ((base & (base - 1)) == 0)
        {
                positive width = base == 2    ? 1
                                 : base == 4  ? 2
                                 : base == 8  ? 3
                                 : base == 16 ? 4
                                              : 5;
                //      How many digits that is, from the highest bit, by
                //      the routine's own arithmetic: floor(bit / 3) is
                //      bit * 43 >> 7 and floor(bit / 5) is bit * 26 >> 7 over
                //      the whole zero to sixty three domain, and the other
                //      three widths are a shift. Written as a division
                //      instead, gcc reaches for a sixty four bit multiply
                //      that writes a register pair, and the field is short
                //      enough that the run is over before it has finished.
                p32 top = top_bit_known(value);
                positive count = width == 1   ? top + 1
                                 : width == 2 ? (top >> 1) + 1
                                 : width == 3 ? ((top * 43) >> 7) + 1
                                 : width == 4 ? (top >> 2) + 1
                                              : ((top * 26) >> 7) + 1;
                p8 address_to write = into + count;

                do
                {
                        positive digit = value & (base - 1);

                        write--;
                        //      Below eleven there are no letters at all, so
                        //      the folded base decides between a table read
                        //      and an add.
                        address_to write = base > 10 ? alphabet[digit]
                                                     : (p8)(48 + digit);
                        value >>= width;
                } while (value);

                return count;
        }

        //      A generic base has no shift to write the field from the right
        //      edge with, so the digits go down in the order they come out
        //      and the run is reversed in place, which is what the routine
        //      does. The divisor is a constant here, so the div the routine
        //      pays once a digit is a multiply and a shift.
        p8 address_to write = into;
        positive count;

        do
        {
                positive digit = value % base;

                address_to write = base > 10 ? alphabet[digit]
                                             : (p8)(48 + digit);
                write++;
                value /= base;
        } while (value);

        count = (positive)(write - into);
        write--;
        for (p8 address_to front = into; front < write; front++, write--)
        {
                p8 keep = address_to front;

                address_to front = address_to write;
                address_to write = keep;
        }
        return count;
}

//====================================================================
//      byte-and-bit
//====================================================================
/*
        Values that are known where the call is written.

        The single-value routines answer from their argument alone, so a
        literal does not shorten one, it removes it: byte_is_digit('7') is
        the token 1 and there is nothing left for the machine to do. Every
        other fold in this file replaces a call with a shorter line of code;
        this is the only one that replaces a call with no line of code.

        And the reach is wider than a literal typed at the call. Because
        these are assembly symbols, the compiler cannot see through one and
        cannot fold a call to one however well it knows the argument -- so
        without the macros at the bottom of this file, isdigit('7') is a call
        at every optimisation level. With them, __builtin_constant_p is
        answered after inlining and constant propagation, so a caller's own
        wrapper folds too: a shell's is_separator('/') and an allocator's
        bits_trailing_zeros(sizeof (struct thing)) both arrive as a constant
        with no call, measured at -O2 and -Os.

        Each expansion is the routine's own arithmetic written in C, so the
        two agree by construction rather than by resemblance, and
        src/test/single.c walks every byte and every single-bit word through
        both to say so.

        Two shapes are avoided on purpose. Nothing here reaches for
        __builtin_popcountll, __builtin_clzll or __builtin_ctzll: riscv64 has
        no Zbb on this baseline and gcc answers all four of those with a call
        into libgcc, which a -nostdlib link has no symbol for, and the two
        count builtins are undefined at zero where these routines are defined
        to answer sixty four. And nothing here narrows to eight bits: the
        argument is an int and may be EOF, or a byte that arrived with its
        high bits still on it, and an expansion that masked would be
        answering a different question than the routine.
*/
static inline INLINE b32 known_is_digit(b32 value)
{ return (p32)(value - 48) < 10; }
static inline INLINE b32 known_is_upper(b32 value)
{ return (p32)(value - 65) < 26; }
static inline INLINE b32 known_is_lower(b32 value)
{ return (p32)(value - 97) < 26; }
static inline INLINE b32 known_is_alpha(b32 value)
{ return (p32)((value | 32) - 97) < 26; }
static inline INLINE b32 known_is_alnum(b32 value)
{ return known_is_alpha(value) | known_is_digit(value); }
static inline INLINE b32 known_is_space(b32 value)
{ return ((p32)(value - 9) < 5) | (value == 32); }
static inline INLINE b32 known_is_hexadecimal(b32 value)
{ return known_is_digit(value) | ((p32)((value | 32) - 97) < 6); }
static inline INLINE b32 known_is_printable(b32 value)
{ return (p32)(value - 32) < 95; }
static inline INLINE b32 known_is_graphic(b32 value)
{ return (p32)(value - 33) < 94; }
static inline INLINE b32 known_is_control(b32 value)
{ return ((p32)value < 32) | (value == 127); }
static inline INLINE b32 known_is_punctuation(b32 value)
{
        return known_is_graphic(value) && !known_is_digit(value)
                                       && !known_is_alpha(value);
}
static inline INLINE b32 known_is_blank(b32 value)
{ return (value == 32) | (value == 9); }
static inline INLINE b32 known_is_ascii(b32 value)
{ return (p32)value < 128; }
static inline INLINE b32 known_to_ascii(b32 value)
{ return value & 127; }
static inline INLINE b32 known_to_upper(b32 value)
{ return value - (known_is_lower(value) << 5); }
static inline INLINE b32 known_to_lower(b32 value)
{ return value + (known_is_upper(value) << 5); }

/*
        Bits folded in pairs, then in nibbles, then in bytes, and the eight
        byte totals summed by one multiply -- the same sequence the riscv64
        body runs, written in C so the compiler folds it away entirely for a
        literal and still has something that links when it cannot.
*/
static inline INLINE b32 known_counted(positive value)
{
        value = value - ((value >> 1) & 0x5555555555555555ull);
        value = (value & 0x3333333333333333ull)
              + ((value >> 2) & 0x3333333333333333ull);
        value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0full;
        return (b32)((value * 0x0101010101010101ull) >> 56);
}

//      One less than an isolated lowest bit is a run of ones exactly as long
//      as the count of trailing zeros, and a word with no bits in it isolates
//      nothing, so zero less one is the whole word and counts sixty four.
static inline INLINE b32 known_trailing_zeros(positive value)
{ return known_counted((value & (positive)(0 - value)) - 1); }

//      Smearing every set bit downward leaves untouched zeros above the
//      highest one; complementing turns those into the ones to count.
static inline INLINE b32 known_leading_zeros(positive value)
{
        value |= value >> 1;  value |= value >> 2;  value |= value >> 4;
        value |= value >> 8;  value |= value >> 16; value |= value >> 32;
        return known_counted(~value);
}

//      The position plus one, and zero for a word with nothing set, which is
//      arrived at by masking the input rather than testing the count.
static inline INLINE b32 known_first_set_wide(bipolar value)
{
        positive word = (positive)value;
        positive present = (positive)0 - (positive)(word != 0);
        return known_counted((word ^ (word - 1)) & present);
}

//      An int arrives sign extended and every bit the extension adds sits
//      above the low thirty two, so it can never be the lowest one.
static inline INLINE b32 known_first_set(b32 value)
{ return known_first_set_wide((bipolar)value); }

//====================================================================
//      fill-and-until
//====================================================================
//      Goes in src/compiler_memory.c, immediately after `fill_known` and before the comment block beginning "A macro names itself in its own replacement".

/*
        A byte that is known where the call is written, and the routines that
        are a length away from one of the three above.

        memory_fill already expands on its size. It does not expand on the
        byte, and the byte is a folded zero far more often than it is anything
        else. Below thirty three the compiler's own memset expansion has the
        constant already and lays it down as an immediate; the hand written
        wide path above it does not, and pays vmovd and vpbroadcastb -- a
        general purpose to vector transfer and a shuffle -- to put a number it
        was told into a register. vpxor is one instruction, breaks the
        dependency on whatever was in the register, and is recognised as
        zeroing by every part that will run this. Three million fills on a
        9950X, expansion as a percent of the routine, lower is quicker:

              bytes   broadcast   vpxor
                 32       29.7%   29.7%   <- below the wide path, the same code
                 33       41.9%   34.0%
                 40       41.4%   34.3%
                 48       41.3%   34.5%
                 64       41.0%   33.8%
                 96       41.2%   29.7%
                127       40.2%   41.2%   <- level again on the four store rung
                128       41.1%   41.8%

        So the broadcast costs about a fifth of the expansion between thirty
        three and ninety six, and nothing above it. A folded byte that is not
        zero was measured too, planted in .rodata and broadcast from memory:
        39.2%, 36.4%, 39.1% and 35.7% against 45.1%, 41.7%, 45.5% and 42.1% at
        thirty three, forty, forty eight and sixty four, but 64.3% against
        49.1% at ninety six, in both runs. A win that reverses inside its own
        range is not a win, and there is no shorter route to a broadcast byte
        than one load, so a folded byte that is not zero gets nothing here.

        Only x86_64 has any of this. KNOWN_WIDE is zero on arm64 and riscv64,
        where the expansion is the compiler's own memset either way and a
        folded byte is already an immediate in it. The one thing worth knowing
        there: at sixty four bytes riscv64 inlines a zero fill as eight stores
        of the zero register and hands a fill of 0x5a back to memset, so the
        folded zero is what makes that size expand at all.

        memory_zero is bzero and is three instructions and a jump into
        memory_fill, so a folded size is fill_known with the byte already
        chosen. It inherits KNOWN_SIZE_MAX rather than needing its own, and it
        measures where the fill it reuses measures: 23% to 42% of the routine
        from nothing to a hundred and twenty eight bytes.

        string_copy_end is stpcpy, and its source is a literal at nearly every
        call site anybody writes. __builtin_strlen folds a literal to a number
        whatever -fno-builtin says, because the double underscore name is a
        builtin by spelling and not by policy, so the length the routine was
        going to walk the string for is already in hand. What is left is a
        copy of that many bytes and the terminator, and an answer that is the
        destination plus a constant. It is the largest win in this family --
        11% to 25% of the routine at every length up to the cutoff -- and the
        cutoff is KNOWN_SIZE_MAX again, because the copy is the whole of the
        work. A source the compiler cannot measure, and a literal longer than
        the cutoff, both arrive at the routine unchanged.

        memory_copy_until is memccpy and is the one that needed its own
        number. The routine asks memory_first_of where the stop byte is and
        hands the span to memory_copy_apart, which is two calls and a frame;
        the expansion writes the scan out, which the compiler can do because
        the size is folded, and copies with the length the scan found. That
        length is not folded, so the copy is written as the overlapping pair
        the class allows -- eight bytes from the front and eight from the back
        cover every length from eight to sixteen and write nothing outside
        them, which matters here more than anywhere else in this file: the
        bytes past the one memccpy stopped on belong to the caller, and
        src/test/standard.c compares the whole destination room against a
        model to say so. Leaving the copy as a call and writing out only the
        scan was measured too and is 84.7% of the routine at sixteen bytes,
        which is not enough to be worth a second body.

        Rotating the stop byte through the span so the branch cannot be
        learned, expansion as a percent of the routine:

              bytes    stop inside    stop absent
                  2          21.6%          14.4%
                  4          33.2%          29.1%
                  8          46.9%          51.1%
                 12          48.3%          64.6%
                 16          63.1%          76.3%
                 20          63.5%          82.7%
                 24          83.2%         104.0%   <- the absent side turns
                 32         111.3%         178.7%

        Which is where KNOWN_UNTIL_MAX comes from. Twenty still wins on both
        sides and is not a size anybody writes; sixteen is the last one that
        is. Above it the routine's vector scan is doing something no straight
        line of byte compares can, and the two calls stop mattering.

        Two ceilings here are load bearing and neither is spelled the same as
        KNOWN_SIZE_MAX, so both are said out loud. copy_end_known is handed a
        length and copies one more than it, so its own ceiling is one below
        the copy's: at a length of exactly KNOWN_SIZE_MAX the copy is a
        hundred and twenty nine bytes, which copy_apart_known takes through
        its widest branch and lands at nought, thirty two, sixty four and
        ninety seven -- byte ninety six is never written. The macro says less
        than where the other three say at most, and that asymmetry is the
        whole of the guard. And copy_short_apart is correct to thirty two
        bytes and no further, which holds only because KNOWN_UNTIL_MAX is
        sixteen; raising it past thirty two copies the ends and leaves the
        middle.
*/
#define KNOWN_UNTIL_MAX 16

#if KNOWN_WIDE
#define KNOWN_ZERO_ASM(body, size, back_offset, ...)                          \
        __asm__("   vpxor %%xmm0, %%xmm0, %%xmm0\n" body "   vzeroupper\n"    \
                : "=m"(*(p8(address_to)[size])(destination))                  \
                : [to] "r"(destination), [back] "i"(back_offset)              \
                : __VA_ARGS__)
#endif

static inline INLINE address_any fill_zero_known(address_any destination,
                                                 positive size)
{
#if KNOWN_WIDE
        if (size > 32 && cpu_has_avx2) {
                if (size <= 64)
                        KNOWN_ZERO_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                else if (size <= 96)
                        KNOWN_ZERO_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, 32(%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                else
                        KNOWN_ZERO_ASM("   vmovdqu %%ymm0, (%[to])\n"
                                       "   vmovdqu %%ymm0, 32(%[to])\n"
                                       "   vmovdqu %%ymm0, 64(%[to])\n"
                                       "   vmovdqu %%ymm0, %c[back](%[to])\n",
                                       size, size - 32, "xmm0");
                return destination;
        }
#endif
        __builtin_memset(destination, 0, size);
        return destination;
}

//      bzero: the byte is chosen already, so only the size has to be folded.
static inline INLINE fn zero_known(address_any destination, positive size)
{
        fill_zero_known(destination, size);
}

//      stpcpy against a source whose length the compiler worked out. The
//      terminator is copied with the rest, and where it lands is the answer.
//      The copy is length plus one, so the caller's guard is a length below
//      KNOWN_SIZE_MAX and not at it.
static inline INLINE p8 address_to copy_end_known(string_address destination,
                                                  string_address source,
                                                  positive length)
{
        copy_apart_known(destination, source, length + 1);
        return destination + length;
}

/*
        A copy whose length the compiler does not know but whose bound it
        does. Each class is a pair of overlapping loads and a pair of
        overlapping stores, so no length needs a remainder and no store lands
        outside the length asked for. The bound is folded, so a caller with a
        small one drops the wider classes altogether rather than branching
        past them.

        The widest class here is thirty two bytes. A length past that would
        copy both ends and leave the middle, so this is not a general copy and
        the only caller it has passes it a bound of KNOWN_UNTIL_MAX.
*/
static inline INLINE fn copy_short_apart(p8 address_to to,
                                         const p8 address_to from,
                                         positive length, positive bound)
{
        if (bound > 16 && length > 16) {
                p64 first, second, third, fourth;

                __builtin_memcpy(&first, from, 8);
                __builtin_memcpy(&second, from + 8, 8);
                __builtin_memcpy(&third, from + length - 16, 8);
                __builtin_memcpy(&fourth, from + length - 8, 8);
                __builtin_memcpy(to, &first, 8);
                __builtin_memcpy(to + 8, &second, 8);
                __builtin_memcpy(to + length - 16, &third, 8);
                __builtin_memcpy(to + length - 8, &fourth, 8);
        } else if (bound >= 8 && length >= 8) {
                p64 head, tail;

                __builtin_memcpy(&head, from, 8);
                __builtin_memcpy(&tail, from + length - 8, 8);
                __builtin_memcpy(to, &head, 8);
                __builtin_memcpy(to + length - 8, &tail, 8);
        } else if (bound >= 4 && length >= 4) {
                p32 head, tail;

                __builtin_memcpy(&head, from, 4);
                __builtin_memcpy(&tail, from + length - 4, 4);
                __builtin_memcpy(to, &head, 4);
                __builtin_memcpy(to + length - 4, &tail, 4);
        } else if (length) {
                //      One, two and three, which the first, the last and the
                //      middle cover between them.
                to[0] = from[0];
                to[length >> 1] = from[length >> 1];
                to[length - 1] = from[length - 1];
        }
}

//      memccpy: the scan written out, and a copy bounded by the same number.
static inline INLINE address_any copy_until_known(address_any destination,
                                                  address_any source,
                                                  b8 value, positive size)
{
        const p8 address_to from = source;

        for (positive at = 0; at < size; at++)
                if (from[at] == value) {
                        copy_short_apart(destination, from, at + 1, size);
                        return (p8 address_to)destination + at + 1;
                }

        //      The byte was not in the span, so the whole span was copied and
        //      the answer memccpy gives is nothing at all.
        copy_apart_known(destination, source, size);
        return null;
}

/*
        A macro names itself in its own replacement, which the preprocessor
        leaves alone: the inner one is the routine, and the call site does not
        change. Function-like, so it only fires where a call is written, and
        taking the address of any of these still names the routine.

        These must come after the prototypes above, which are the same names
        followed by an open bracket and would expand.
*/
#define memory_copy(destination, source, size)                                \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? copy_known((destination), (source), (size))                \
                 : memory_copy((destination), (source), (size)))

#define memory_copy_apart(destination, source, size)                           \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? copy_apart_known((destination), (source), (size))           \
                 : memory_copy_apart((destination), (source), (size)))

#define memory_fill(destination, value, size)                                 \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? fill_known((destination), (value), (size))                 \
                 : memory_fill((destination), (value), (size)))


//      set-literal
/*
        A macro names itself in its own replacement, which the preprocessor
        leaves alone: the inner one is the routine, and a set the compiler
        cannot read arrives there unchanged. Function-like, so it only fires
        where a call is written and taking the address of any of these still
        names the routine.

        These must come after the prototypes, which are the same names
        followed by an open bracket and would expand.
*/
#define string_span_of_set(source, accept)                                    \
        (!set_known(accept)                                                   \
                 ? string_span_of_set((source), (accept))                     \
         : set_known_length(accept) == 0                                      \
                 ? ((void)(source), (positive)0)                              \
                 : string_span((source),                                      \
                               set_known_table(accept, set_known_holds)))

#define string_span_without_set(source, reject)                               \
        (!set_known(reject)                                                   \
                 ? string_span_without_set((source), (reject))                \
         : set_known_length(reject) == 0                                      \
                 ? string_length(source)                                      \
         : set_known_length(reject) == 1                                      \
                 ? span_without_byte_known((source), (p8)(reject)[0])         \
                 : string_span((source),                                      \
                               set_known_table(reject, set_known_stops)))

#define string_first_of_set(source, accept)                                   \
        (!set_known(accept)                                                   \
                 ? string_first_of_set((source), (accept))                    \
         : set_known_length(accept) == 0                                      \
                 ? ((void)(source), (string_address)null)                     \
         : set_known_length(accept) == 1                                      \
                 ? string_first_of((source), (p8)(accept)[0])                 \
                 : first_of_set_known((source),                                   \
                                  set_known_table(accept, set_known_stops)))

//      needle-length
#define memory_search(block, size, needle, needle_size)                       \
        (__builtin_constant_p(needle_size) &&                                 \
                         (positive)(needle_size) <= KNOWN_NEEDLE_MAX          \
                 ? search_known((block), (size), (needle), (needle_size))     \
                 : memory_search((block), (size), (needle), (needle_size)))

#define memory_search_ascii_case(block, size, needle, needle_size)            \
        (__builtin_constant_p(needle_size) &&                                 \
                         (positive)(needle_size) <= KNOWN_NEEDLE_MAX          \
                 ? search_case_known((block), (size), (needle), (needle_size)) \
                 : memory_search_ascii_case((block), (size), (needle),        \
                                            (needle_size)))

//      bounded-string
//      Add at the bottom of src/compiler_memory.c, alongside the memory_copy / memory_copy_apart / memory_fill macros, after the prototypes above. Durable copy: /private/tmp/claude-501/-Users-alve-dawning-kit/96a48472-b59c-4e34-b254-8c9750e693c6/scratchpad/block_macros.c

#define string_length_max(source, bound)                                      \
        (__builtin_constant_p(bound) && (positive)(bound) <= KNOWN_BOUND_MAX  \
                 ? length_max_known((source), (bound))                        \
                 : string_length_max((source), (bound)))

#define string_compare_max(source, input, bound)                              \
        (__builtin_constant_p(bound) && (positive)(bound) <= KNOWN_BOUND_MAX  \
                 ? compare_max_known((source), (input), (bound))              \
                 : string_compare_max((source), (input), (bound)))

#define string_copy_max_end(into, source, bound)                              \
        (__builtin_constant_p(bound) && (positive)(bound) <= KNOWN_PAIR_MAX   \
                 ? copy_max_end_known((into), (source), (bound))              \
                 : string_copy_max_end((into), (source), (bound)))

#define string_copy_max_endptr(destination, source, bound)                    \
        (__builtin_constant_p(bound) && (positive)(bound) <= KNOWN_PAIR_MAX   \
                 ? copy_max_endptr_known((destination), (source), (bound))    \
                 : string_copy_max_endptr((destination), (source), (bound)))

#define string_append_max(destination, source, bound)                         \
        (__builtin_constant_p(bound) && (positive)(bound) <= KNOWN_APPEND_MAX \
                 ? append_max_known((destination), (source), (bound))         \
                 : string_append_max((destination), (source), (bound)))

//      There is deliberately no string_copy_max macro. See the verdict and the block's prose.

//      compare-and-find
//      These go at the bottom of src/compiler_memory.c, beside the three that are there, after all the prototypes:

#define memory_compare(first, second, size)                                   \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_COMPARE_MAX  \
                 ? compare_known((first), (second), (size))                   \
                 : memory_compare((first), (second), (size)))

#define memory_first_of(block, value, size)                                   \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SCAN_MAX     \
                 ? first_of_known((block), (value), (size))                   \
                 : memory_first_of((block), (value), (size)))

#define memory_last_of(block, value, size)                                    \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SCAN_MAX     \
                 ? last_of_known((block), (value), (size))                    \
                 : memory_last_of((block), (value), (size)))

//      number-base
/*
        Which bases expand, and which are the call.

        positive_into_base takes every base it accepts, because there is no
        length at which any of them loses: ten is the decimal engine reached
        without the deciding, a power of two is a folded mask and a folded
        shift, and anything else divides by a constant, which is a multiply.

        The parse takes the bases with no letters in them, and sixteen. Below
        eleven a byte is a digit or it is the end of the run, and one subtract
        and one unsigned compare say which -- so the two alphabet ranges, the
        branch between them and the base compare all go, and the expansion is
        between 53% and 98% of the routine at every length measured. Sixteen
        keeps the letters but accumulates by shift instead of by multiply,
        which is level to six digits and 74% at sixteen of them. The bases in
        between, and above, keep the letters and keep the multiply, and there
        the expansion measured 100% to 109% from four digits up -- so they are
        the ordinary call, and base zero, which asks for the prefix to be
        detected rather than folded, is the ordinary call too.
*/
#define KNOWN_BASE_PARSES(base) \
        (((base) >= 2 && (base) <= 10) || (base) == 16)

#define string_to_number(input, stopped, base)                                \
        (__builtin_constant_p(base) && KNOWN_BASE_PARSES(base)                \
                 ? (bipolar)number_known((input), (stopped),                  \
                                         (positive)(base), 1)                 \
                 : string_to_number((input), (stopped), (base)))

#define string_to_number_unsigned(input, stopped, base)                       \
        (__builtin_constant_p(base) && KNOWN_BASE_PARSES(base)                \
                 ? number_known((input), (stopped), (positive)(base), 0)      \
                 : string_to_number_unsigned((input), (stopped), (base)))

#define positive_into_base(into, value, base, upper)                          \
        (__builtin_constant_p(base) && (positive)(base) - 2 <= 34             \
                 ? into_base_known((into), (value), (positive)(base),         \
                                   (upper))                                   \
                 : positive_into_base((into), (value), (base), (upper)))

//      byte-and-bit
/*
        Twenty one macros naming themselves, through one shared shape.

        KNOWN_SINGLE takes the routine's name as an argument and puts it back
        in the else arm, which is safe for the same reason the three above are
        written out in full: a name that came from its own macro's replacement
        is marked as not to be replaced again, and that marking follows the
        token through the nested expansion it is handed to. Do not fold the
        indirection away by hand -- the reason it works is a rule about
        rescanning, not about how the text looks, and the check that it does
        work is the unfolded call in src/test/single.c.

        Function-like, so each only fires where a call is written, and a bare
        name still takes the address of the routine.
*/
#define KNOWN_SINGLE(name, known, value)                                      \
        (__builtin_constant_p(value) ? known(value) : name(value))

#define byte_is_digit(value)       KNOWN_SINGLE(byte_is_digit, known_is_digit, (value))
#define byte_is_upper(value)       KNOWN_SINGLE(byte_is_upper, known_is_upper, (value))
#define byte_is_lower(value)       KNOWN_SINGLE(byte_is_lower, known_is_lower, (value))
#define byte_is_alpha(value)       KNOWN_SINGLE(byte_is_alpha, known_is_alpha, (value))
#define byte_is_alnum(value)       KNOWN_SINGLE(byte_is_alnum, known_is_alnum, (value))
#define byte_is_space(value)       KNOWN_SINGLE(byte_is_space, known_is_space, (value))
#define byte_is_hexadecimal(value) KNOWN_SINGLE(byte_is_hexadecimal, known_is_hexadecimal, (value))
#define byte_is_printable(value)   KNOWN_SINGLE(byte_is_printable, known_is_printable, (value))
#define byte_is_graphic(value)     KNOWN_SINGLE(byte_is_graphic, known_is_graphic, (value))
#define byte_is_control(value)     KNOWN_SINGLE(byte_is_control, known_is_control, (value))
#define byte_is_punctuation(value) KNOWN_SINGLE(byte_is_punctuation, known_is_punctuation, (value))
#define byte_is_blank(value)       KNOWN_SINGLE(byte_is_blank, known_is_blank, (value))
#define byte_is_ascii(value)       KNOWN_SINGLE(byte_is_ascii, known_is_ascii, (value))
#define byte_to_ascii(value)       KNOWN_SINGLE(byte_to_ascii, known_to_ascii, (value))
#define byte_to_upper(value)       KNOWN_SINGLE(byte_to_upper, known_to_upper, (value))
#define byte_to_lower(value)       KNOWN_SINGLE(byte_to_lower, known_to_lower, (value))
#define bits_counted(value)        KNOWN_SINGLE(bits_counted, known_counted, (value))
#define bits_trailing_zeros(value) KNOWN_SINGLE(bits_trailing_zeros, known_trailing_zeros, (value))
#define bits_leading_zeros(value)  KNOWN_SINGLE(bits_leading_zeros, known_leading_zeros, (value))
#define bits_first_set(value)      KNOWN_SINGLE(bits_first_set, known_first_set, (value))
#define bits_first_set_wide(value) KNOWN_SINGLE(bits_first_set_wide, known_first_set_wide, (value))

//      fill-and-until
//      The existing `memory_fill` macro is replaced; the other three are new. All four go at the bottom of src/compiler_memory.c after the prototypes, in the block that already holds `memory_copy` and `memory_copy_apart`.

#define memory_fill(destination, value, size)                                 \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? (__builtin_constant_p(value) && (b8)(value) == 0           \
                            ? fill_zero_known((destination), (size))          \
                            : fill_known((destination), (value), (size)))     \
                 : memory_fill((destination), (value), (size)))

#define memory_zero(destination, size)                                        \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? zero_known((destination), (size))                          \
                 : memory_zero((destination), (size)))

#define memory_copy_until(destination, source, value, size)                   \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_UNTIL_MAX    \
                 ? copy_until_known((destination), (source), (value), (size)) \
                 : memory_copy_until((destination), (source), (value), (size)))

#define string_copy_end(destination, source)                                  \
        (__builtin_constant_p(__builtin_strlen((const char address_to)(source))) \
         && __builtin_strlen((const char address_to)(source)) < KNOWN_SIZE_MAX \
                 ? copy_end_known((destination), (source),                    \
                                  __builtin_strlen((const char address_to)(source))) \
                 : string_copy_end((destination), (source)))

//      Note the `<` in string_copy_end where the other three have `<=`. That is not a typo and it is not an inconsistency to tidy: copy_end_known copies length plus one, so a length of exactly KNOWN_SIZE_MAX asks copy_apart_known for 129 bytes, which is past what it is correct for.
//      memory_copy_until guards on the size alone and never on the byte. Folding the byte buys nothing — the compare is register against an immediate or against a register, and the length is the same either way — while requiring both would halve the number of call sites the expansion reaches.

#endif // STANDARD_MODERN_C_COMPILER_MEMORY
