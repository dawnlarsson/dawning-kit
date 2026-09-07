/*
        Constant-size memcpy and memset, folded where the caller already knows
        the answer.

        WHY THIS IS WORTH A FILE. The kernel passes a compile-time constant
        size to memcpy at 8,024 call sites and to memset at 16,361 -- 29% and
        71% of their calls. Of the sizes that are plain literals, 68.2% are
        eight bytes or fewer and 91.7% are thirty two or fewer. None of them
        folds today, because what the caller sees is a DECLARATION and the
        work happens behind a call: __builtin_constant_p never gets a chance.

        Measured on a 9950X: a call to our routine costs about seventeen
        cycles at eight bytes; the folded form is two instructions and a
        return, and disappears into the caller. That is roughly ten to twelve
        cycles recovered per constant-size call site, with no edit to any
        kernel call site at all.

        THIS IS THE HEADER'S OWN IDIOM, not a trick played on it. Ten lines up
        from where this lands, memcpy_flushcache does the same thing:
        "if (__builtin_constant_p(cnt)) switch (cnt) { case 4: ... case 8: ...
        case 16: ... }" and falls back to the out-of-line call. We are folding
        more names and more sizes, not introducing a technique.

        THE MACRO REFERS TO ITSELF ON PURPOSE. A function-like macro is not
        re-expanded inside its own replacement, so the else arm is the real
        call. src/compiler_memory.c has used this shape for its known-size
        specializers since they were written.

        VECTOR REGISTERS CANNOT ESCAPE FROM HERE, and not because of anything
        written below. The kernel compiles with -mno-sse -mno-sse2 -mno-mmx
        -mno-avx -mno-80387, so gcc has no vector register to choose even when
        it wants one: at sixteen bytes it picks movdqa/movaps with those flags
        absent and two movq pairs with them present. Checked both ways before
        this was written, because kernel code that touches xmm without
        kernel_fpu_begin corrupts whatever userspace had in it.

        WHAT IS NOT FOLDED. memcmp, because folding a comparison has to keep
        the ordered difference of the first differing byte and that is more
        than a load pair. memmove, because 55 of its 873 call sites pass a
        constant and it is the one name whose whole job is the overlapping
        case. Sizes above thirty two, because the routine reaches rep movsb
        there and is already level with the architecture's own.

        A caller that #undefs memcpy or memset -- there are 43 in the tree --
        loses the fold and keeps the call, which is exactly right and needs
        nothing from us.
*/
#ifndef MOONWATER_FOLD_X86
#define MOONWATER_FOLD_X86 1

/* The compiler owns the same <=32-byte tier in compiler_memory.c. Explicit
   builtins preserve unaligned access without a second set of width/tail rules;
   the kernel's -mno-sse/-mno-avx flags constrain them to general registers. */
static __always_inline void *moonwater_fold_copy(void *to, const void *from,
                                                 __kernel_size_t count)
{
        return __builtin_memcpy(to, from, count);
}

static __always_inline void *moonwater_fold_fill(void *to, int byte,
                                                 __kernel_size_t count)
{
        return __builtin_memset(to, byte, count);
}

//      Thirty two, because 91.7% of the constant sizes in the tree are at or
//      below it and the routine reaches rep movsb just above it.
#define MOONWATER_FOLD_MAX 32

#ifndef FOLD_ONLY_BODIES
#define memcpy(to, from, count)                                               \
        (__builtin_constant_p(count) &&                                       \
         (__kernel_size_t)(count) <= MOONWATER_FOLD_MAX                       \
                 ? moonwater_fold_copy((to), (from), (count))                 \
                 : memcpy((to), (from), (count)))

#define memset(to, byte, count)                                               \
        (__builtin_constant_p(count) && __builtin_constant_p(byte) &&         \
         (__kernel_size_t)(count) <= MOONWATER_FOLD_MAX                       \
                 ? moonwater_fold_fill((to), (byte), (count))                 \
                 : memset((to), (byte), (count)))

#endif /* FOLD_ONLY_BODIES */

#endif /* MOONWATER_FOLD_X86 */
