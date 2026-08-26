#
#       canvas_row_fill and canvas_row_blit -- one run of pixels.
#
#       Every pixel Canvas puts on a screen goes through one of these: the
#       desktop, a window body, a titlebar, every lit pixel of every glyph. A
#       full compose of one 1280x800 output is four megabytes of stores, and
#       the kernel is built -mno-sse on x86, so what gcc emitted for the C was
#       two four byte stores per iteration and no vector anything:
#
#           mov %ecx,(%rdx,%rax,4)
#           mov %ecx,0x4(%rdx,%rax,4)
#           add $0x2,%rax
#           cmp %eax,%esi
#           jg  loop
#
#       Eight bytes for five instructions. That is the shape the string
#       functions had, not the shape find_bit had.
#
#       void canvas_row_fill(u32 *at, unsigned long count, u32 colour)
#       void canvas_row_blit(u32 *at, const u32 *from, unsigned long count,
#                            u32 opaque)
#       void canvas_rect_fill(u32 *at, unsigned long pitch, unsigned long width,
#                             unsigned long height, u32 colour)
#
#       The rectangle is the one that matters. A window's two sides are two
#       pixels wide and a hundred and ninety rows tall, so a row at a time was
#       4560 calls a compose to write 2 pixels each; a desktop was 800 calls.
#       All the row walking is in here now, and the decision about how to fill
#       a row is taken once for the whole rectangle rather than once a row.
#
#include <linux/export.h>
#include <linux/linkage.h>

        .text


#> arch x86_64
SYM_FUNC_START(canvas_row_fill)
        #
        #       Three sizes, and the smallest falls through, because it is the
        #       one called most often: a glyph asks for a run of a few pixels
        #       and asks thousands of times, while a desktop asks once.
        #
        #       The first pixel and the last are written without asking how
        #       many there are. For one they are the same pixel twice, which
        #       costs less than the branch that would avoid it.
        #
        cmp     $4, %rsi
        jae     3f

        test    %rsi, %rsi
        jz      9f
        mov     %edx, (%rdi)
        mov     %edx, -4(%rdi,%rsi,4)
        cmp     $3, %rsi
        jb      9f
        mov     %edx, 4(%rdi)
9:      RET

3:      mov     %edx, %eax
        mov     %eax, %r8d
        shl     $32, %r8
        or      %r8, %rax               # the colour twice, for eight byte stores

        #
        #       rep stos is the fastest thing here for a desktop sized fill
        #       and the slowest for a small one: it costs tens of cycles to
        #       start whatever the count. Measured, it does not overtake a
        #       plain loop until about two hundred pixels.
        #
        cmp     $256, %rsi
        jae     5f

        sub     $4, %rsi
        jb      2f
1:      mov     %rax, (%rdi)            # four pixels an iteration
        mov     %rax, 8(%rdi)
        add     $16, %rdi
        sub     $4, %rsi
        jae     1b
2:      add     $4, %rsi
        jz      8f

        test    $2, %sil
        jz      4f
        mov     %rax, (%rdi)
        add     $8, %rdi
4:      test    $1, %sil
        jz      8f
        mov     %eax, (%rdi)
8:      RET

5:      mov     %rsi, %rcx
        shr     $1, %rcx
        rep stosq                       # advances %rdi past what it wrote

        test    $1, %sil
        jz      6f
        mov     %eax, (%rdi)
6:      RET
SYM_FUNC_END(canvas_row_fill)

#> arch arm64
SYM_FUNC_START(canvas_row_fill)
        mov     w2, w2                  // zero the top half, which the ABI
        orr     x3, x2, x2, lsl #32     // leaves undefined for a 32 bit argument

        cmp     x1, #4
        b.lo    2f

1:      stp     x3, x3, [x0], #16       // four pixels
        sub     x1, x1, #4
        cmp     x1, #4
        b.hs    1b

2:      tbz     x1, #1, 3f
        str     x3, [x0], #8
3:      tbz     x1, #0, 4f
        str     w2, [x0]
4:      ret
SYM_FUNC_END(canvas_row_fill)

#> arch riscv64
SYM_FUNC_START(canvas_row_fill)
        slli    a2, a2, 32
        srli    a2, a2, 32              # zero extend the colour
        slli    t0, a2, 32
        or      t0, t0, a2              # the colour twice

        li      t1, 2
        blt     a1, t1, 2f

        #
        #       A pixel is four bytes, so a run can start halfway through the
        #       eight the wide path stores. Misaligned there is a trap on the
        #       cores that do not take it and a slow path on the ones that do,
        #       so the odd first pixel goes on its own.
        #
        andi    t2, a0, 7
        beqz    t2, 1f
        sw      a2, 0(a0)
        addi    a0, a0, 4
        addi    a1, a1, -1
        blt     a1, t1, 2f

1:      sd      t0, 0(a0)
        addi    a0, a0, 8
        addi    a1, a1, -2
        bge     a1, t1, 1b

2:      beqz    a1, 3f
        sw      a2, 0(a0)
3:      ret
SYM_FUNC_END(canvas_row_fill)

#> arch other

#> shared


#> arch x86_64
SYM_FUNC_START(canvas_rect_fill)
        test    %rcx, %rcx
        jz      9f
        test    %rdx, %rdx
        jz      9f

        shl     $2, %rsi                # pitch, pixels to bytes
        mov     %r8d, %eax
        mov     %eax, %r11d
        shl     $32, %r11
        or      %r11, %rax              # the colour twice
        mov     %rdi, %r9               # where this row starts
        mov     %rcx, %r10              # rows left

        cmp     $256, %rdx
        jae     5f

        #
        #       Narrow. Four pixels an iteration, and the two pixel case that
        #       a window's side is takes the tail alone.
        #
1:      mov     %r9, %rdi
        mov     %rdx, %rcx
        sub     $4, %rcx
        jb      3f
2:      mov     %rax, (%rdi)
        mov     %rax, 8(%rdi)
        add     $16, %rdi
        sub     $4, %rcx
        jae     2b
3:      add     $4, %rcx
        jz      4f
        test    $2, %cl
        jz      6f
        mov     %rax, (%rdi)
        add     $8, %rdi
6:      test    $1, %cl
        jz      4f
        mov     %eax, (%rdi)
4:      add     %rsi, %r9
        dec     %r10
        jnz     1b
9:      RET

        # Wide enough that starting a rep costs less than the stores it saves.
5:      mov     %r9, %rdi
        mov     %rdx, %rcx
        shr     $1, %rcx
        rep stosq
        test    $1, %dl
        jz      7f
        mov     %eax, (%rdi)
7:      add     %rsi, %r9
        dec     %r10
        jnz     5b
        RET
SYM_FUNC_END(canvas_rect_fill)

#> arch arm64
SYM_FUNC_START(canvas_rect_fill)
        cbz     x3, 9f
        cbz     x2, 9f

        lsl     x1, x1, #2
        mov     w4, w4
        orr     x5, x4, x4, lsl #32

1:      mov     x6, x0                  // where this row starts
        mov     x7, x2                  // pixels left in it

        cmp     x7, #4
        b.lo    3f
2:      stp     x5, x5, [x6], #16
        sub     x7, x7, #4
        cmp     x7, #4
        b.hs    2b

3:      tbz     x7, #1, 4f
        str     x5, [x6], #8
4:      tbz     x7, #0, 5f
        str     w4, [x6]

5:      add     x0, x0, x1
        subs    x3, x3, #1
        b.ne    1b
9:      ret
SYM_FUNC_END(canvas_rect_fill)

#> arch riscv64
SYM_FUNC_START(canvas_rect_fill)
        beqz    a3, 9f
        beqz    a2, 9f

        slli    a1, a1, 2
        slli    a4, a4, 32
        srli    a4, a4, 32
        slli    t0, a4, 32
        or      t0, t0, a4
        li      t3, 2

1:      mv      t1, a0
        mv      t2, a2

        blt     t2, t3, 3f

        # A row at a time, because an odd pitch puts every other row half a
        # pair out of step with the one above it.
        andi    t4, t1, 7
        beqz    t4, 2f
        sw      a4, 0(t1)
        addi    t1, t1, 4
        addi    t2, t2, -1
        blt     t2, t3, 3f

2:      sd      t0, 0(t1)
        addi    t1, t1, 8
        addi    t2, t2, -2
        bge     t2, t3, 2b

3:      beqz    t2, 4f
        sw      a4, 0(t1)

4:      add     a0, a0, a1
        addi    a3, a3, -1
        bnez    a3, 1b
9:      ret
SYM_FUNC_END(canvas_rect_fill)

#> arch other

#> shared


#> arch x86_64
SYM_FUNC_START(canvas_row_blit)
        test    %ecx, %ecx
        jnz     5f

        # No alpha to force on, so this is a copy, and past the point where
        # starting one pays that is what rep movs is for.
        cmp     $256, %rdx
        jb      5f

        mov     %rdx, %rcx
        shr     $1, %rcx
        rep movsq

        test    $1, %dl
        jz      9f
        mov     (%rsi), %eax
        mov     %eax, (%rdi)
        RET

5:      mov     %ecx, %eax
        shl     $32, %rcx
        or      %rax, %rcx              # the mask twice

        sub     $2, %rdx
        jb      7f
6:      mov     (%rsi), %r8             # two pixels an iteration
        or      %rcx, %r8
        mov     %r8, (%rdi)
        add     $8, %rsi
        add     $8, %rdi
        sub     $2, %rdx
        jae     6b
7:      test    $1, %dl
        jz      9f
        mov     (%rsi), %r8d
        or      %eax, %r8d
        mov     %r8d, (%rdi)
9:      RET
SYM_FUNC_END(canvas_row_blit)

#> arch arm64
SYM_FUNC_START(canvas_row_blit)
        mov     w3, w3
        orr     x4, x3, x3, lsl #32

        cmp     x2, #2
        b.lo    2f

1:      ldr     x5, [x1], #8            // two pixels
        orr     x5, x5, x4
        str     x5, [x0], #8
        sub     x2, x2, #2
        cmp     x2, #2
        b.hs    1b

2:      cbz     x2, 3f
        ldr     w5, [x1]
        orr     w5, w5, w3
        str     w5, [x0]
3:      ret
SYM_FUNC_END(canvas_row_blit)

#> arch riscv64
SYM_FUNC_START(canvas_row_blit)
        slli    a3, a3, 32
        srli    a3, a3, 32
        slli    t0, a3, 32
        or      t0, t0, a3

        li      t1, 2
        blt     a2, t1, 2f

        # Both ends have to be eight byte aligned for the pair path, and a
        # pixel pointer is only aligned to four, so one pixel goes first to
        # bring the destination up. A source that is still out of step after
        # that cannot be brought into it, and goes a pixel at a time.
        andi    t2, a0, 7
        beqz    t2, 4f
        lw      t2, 0(a1)
        or      t2, t2, a3
        sw      t2, 0(a0)
        addi    a1, a1, 4
        addi    a0, a0, 4
        addi    a2, a2, -1
        blt     a2, t1, 2f

4:      andi    t2, a1, 7
        bnez    t2, 5f

1:      ld      t2, 0(a1)
        or      t2, t2, t0
        sd      t2, 0(a0)
        addi    a1, a1, 8
        addi    a0, a0, 8
        addi    a2, a2, -2
        bge     a2, t1, 1b

2:      beqz    a2, 3f
        lw      t2, 0(a1)
        or      t2, t2, a3
        sw      t2, 0(a0)
3:      ret

5:      lw      t2, 0(a1)
        or      t2, t2, a3
        sw      t2, 0(a0)
        addi    a1, a1, 4
        addi    a0, a0, 4
        addi    a2, a2, -1
        bnez    a2, 5b
        ret
SYM_FUNC_END(canvas_row_blit)

#> arch other

#> shared
