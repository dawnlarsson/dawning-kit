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

        UNALIGNED IS THE POINT. The kernel's callers hand out structure
        members and packed fields, so a plain "*(u32 *)dst = *(const u32 *)src"
        would tell gcc an alignment it does not have. The packed may_alias
        structures below are how the rest of the kernel spells an unaligned
        access, and they compile to the same instruction on x86_64.

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

struct moonwater_una_16 { __u16 value; } __attribute__((packed, may_alias));
struct moonwater_una_32 { __u32 value; } __attribute__((packed, may_alias));
struct moonwater_una_64 { __u64 value; } __attribute__((packed, may_alias));

#define MOONWATER_GET(width, from)                                            \
        (((const struct moonwater_una_##width *)(from))->value)
#define MOONWATER_PUT(width, to, what)                                        \
        (((struct moonwater_una_##width *)(to))->value = (what))

static __always_inline void *moonwater_fold_copy(void *to, const void *from,
                                                 __kernel_size_t count)
{
        unsigned char *at = (unsigned char *)to;
        const unsigned char *source = (const unsigned char *)from;

        switch (count) {
        case 0:  break;
        case 1:  *at = *source; break;
        case 2:  MOONWATER_PUT(16, at, MOONWATER_GET(16, source)); break;
        case 4:  MOONWATER_PUT(32, at, MOONWATER_GET(32, source)); break;
        case 8:  MOONWATER_PUT(64, at, MOONWATER_GET(64, source)); break;
        case 3:  case 5:  case 6:  case 7:
                /*
                        THE OVERLAPPING TAIL BELOW CANNOT BE USED HERE, and
                        the first version of this file used it anyway.

                        It finishes a block by moving the LAST eight bytes,
                        which overlaps what it already moved. That is only a
                        move backwards while the count is at least eight. At
                        three it is "at + 3 - 8", five bytes BEFORE the
                        destination -- and it wrote there. The kernel took a
                        general protection fault on a non-canonical address
                        in PID 1 before the first shell, with RAX holding a
                        pointer that had had its top half overwritten.

                        So the short odd sizes are moved four-then-remainder,
                        both overlapping forwards inside the region and never
                        reaching outside it.
                */
                if (count >= 4) {
                        MOONWATER_PUT(32, at, MOONWATER_GET(32, source));
                        MOONWATER_PUT(32, at + count - 4,
                                      MOONWATER_GET(32, source + count - 4));
                } else {
                        MOONWATER_PUT(16, at, MOONWATER_GET(16, source));
                        at[count - 1] = source[count - 1];
                }
                break;
        default:
                /*
                        Eight or more: whole eight byte moves, then one
                        overlapping move for the remainder, which is how the
                        routine itself finishes a block and costs one extra
                        move rather than a branch per tail byte. Safe here
                        because count is at least eight, so the overlapping
                        move starts at or after the destination.
                */
                {
                        __kernel_size_t done = 0;

                        while (done + 8 <= count) {
                                MOONWATER_PUT(64, at + done,
                                              MOONWATER_GET(64, source + done));
                                done += 8;
                        }

                        if (done != count)
                                MOONWATER_PUT(64, at + count - 8,
                                              MOONWATER_GET(64, source + count - 8));
                }
                break;
        }

        return to;
}

static __always_inline void *moonwater_fold_fill(void *to, int byte,
                                                 __kernel_size_t count)
{
        unsigned char *at = (unsigned char *)to;
        __u64 spread = (__u64)(unsigned char)byte * 0x0101010101010101ULL;

        switch (count) {
        case 0:  break;
        case 1:  *at = (unsigned char)byte; break;
        case 2:  MOONWATER_PUT(16, at, (__u16)spread); break;
        case 4:  MOONWATER_PUT(32, at, (__u32)spread); break;
        case 8:  MOONWATER_PUT(64, at, spread); break;
        case 3:  case 5:  case 6:  case 7:
                //      Short odd sizes, for the reason written out in the
                //      copy above: the overlapping tail reaches behind the
                //      destination when the count is under eight.
                if (count >= 4) {
                        MOONWATER_PUT(32, at, (__u32)spread);
                        MOONWATER_PUT(32, at + count - 4, (__u32)spread);
                } else {
                        MOONWATER_PUT(16, at, (__u16)spread);
                        at[count - 1] = (unsigned char)byte;
                }
                break;
        default:
                {
                        __kernel_size_t done = 0;

                        while (done + 8 <= count) {
                                MOONWATER_PUT(64, at + done, spread);
                                done += 8;
                        }

                        if (done != count)
                                MOONWATER_PUT(64, at + count - 8, spread);
                }
                break;
        }

        return to;
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
