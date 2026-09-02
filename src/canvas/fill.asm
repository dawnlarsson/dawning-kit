#
#       Canvas rectangles and alpha blits.
#
#       Large rectangles and copied pixel windows go through these. Short
#       solid runs share memory_fill_u32 in library.c, while glyph expansion
#       lives in glyph.asm. A full compose of one 1280x800 output is four
#       megabytes of stores, and
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

        # The overwhelmingly common narrow rectangle is one scale-one window
        # border.  Keep its two stores out of the generic tail ladder.
        cmp     $2, %rdx
        je      8f

        # The shared u32-fill floor crosses at 208 words on native Zen 5.
        # This is the same row traffic and therefore the same threshold.
        cmp     $208, %rdx
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

8:      mov     %rax, (%r9)
        add     %rsi, %r9
        dec     %r10
        jnz     8b
        RET

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

        cmp     x2, #2
        b.eq    8f

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

8:      str     x5, [x0]
        add     x0, x0, x1
        subs    x3, x3, #1
        b.ne    8b
        ret
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

        beq     a2, t3, 8f

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

        # A u32 row need only be four-byte aligned on baseline RISC-V, so the
        # two pixels stay two word stores rather than one potentially
        # misaligned doubleword store.
8:      sw      a4, 0(a0)
        sw      a4, 4(a0)
        add     a0, a0, a1
        addi    a3, a3, -1
        bnez    a3, 8b
        ret
SYM_FUNC_END(canvas_rect_fill)

#> arch other

#> shared


#> arch x86_64
SYM_FUNC_START(canvas_row_blit)
        test    %ecx, %ecx
        jnz     5f

        # XRGB needs no alpha fixup.  The common copy floor already owns the
        # size/alignment ladder and ERMS crossover, so do not carry a second,
        # less complete copy engine here.
        shl     $2, %rdx
        jmp     memory_copy_apart

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
        cbnz    w3, 4f
        lsl     x2, x2, #2
        b       memory_copy_apart

4:
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
        bnez    a3, 4f
        slli    a2, a2, 2
        tail    memory_copy_apart

4:
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
