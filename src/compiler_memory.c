
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

static inline INLINE address_any copy_fast_known(address_any destination,
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

#define memory_copy_fast(destination, source, size)                           \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? copy_fast_known((destination), (source), (size))           \
                 : memory_copy_fast((destination), (source), (size)))

#define memory_fill(destination, value, size)                                 \
        (__builtin_constant_p(size) && (positive)(size) <= KNOWN_SIZE_MAX     \
                 ? fill_known((destination), (value), (size))                 \
                 : memory_fill((destination), (value), (size)))

#endif // STANDARD_MODERN_C_COMPILER_MEMORY
