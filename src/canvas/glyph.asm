#
#       canvas_glyph -- one glyph, eight pixels wide, scale one.
#
#       Text was measured at 45% of a compose while drawing about 2.5% of the
#       pixels. The cost was never the pixels: a glyph row was scanned bit by
#       bit for runs, each run clipped, and each run a call. Fifteen or so
#       calls per glyph, thousands of glyphs.
#
#       This draws a whole glyph in one call. The caller has already decided
#       the glyph is entirely inside the damage, which is the ordinary case;
#       one that straddles the edge still goes the long way round.
#
#       void canvas_glyph(u32 *at, unsigned long pitch, const u8 *bits,
#                         unsigned long stride, unsigned long rows, u32 colour)
#       void canvas_cell(u32 *at, unsigned long pitch, const u8 *bits,
#                        unsigned long rows, u32 ink, u32 paper)
#
#       canvas_cell is the same glyph with its background, written in one pass.
#       There is one framebuffer and the display is reading it, so filling the
#       paper and then drawing the glyph over it is every letter on the screen
#       flashing its background whenever it is repainted. Every pixel here is
#       stored once, already the colour it ends up.
#
#       pitch is in pixels. bits is one byte a row, most significant bit
#       leftmost, which is how the kernel's console fonts are stored, and
#       stride is how far apart those bytes are. A font row is one byte, so
#       stride is one; the cursor is sixteen wide and drawn as two halves of a
#       two byte row, so stride is two and the second call starts a byte along
#       and eight pixels over.
#
#include <linux/export.h>
#include <linux/linkage.h>

        .text


#> arch x86_64
SYM_FUNC_START(canvas_glyph)
        shl     $2, %rsi                # pitch, pixels to bytes

1:      movzbl  (%rdx), %eax
        add     %rcx, %rdx
        test    %eax, %eax
        jz      2f                      # a blank row, and most rows are

        test    $0x80, %al
        jz      3f
        mov     %r9d, (%rdi)
3:      test    $0x40, %al
        jz      4f
        mov     %r9d, 4(%rdi)
4:      test    $0x20, %al
        jz      5f
        mov     %r9d, 8(%rdi)
5:      test    $0x10, %al
        jz      6f
        mov     %r9d, 12(%rdi)
6:      test    $0x08, %al
        jz      7f
        mov     %r9d, 16(%rdi)
7:      test    $0x04, %al
        jz      8f
        mov     %r9d, 20(%rdi)
8:      test    $0x02, %al
        jz      9f
        mov     %r9d, 24(%rdi)
9:      test    $0x01, %al
        jz      2f
        mov     %r9d, 28(%rdi)

2:      add     %rsi, %rdi
        dec     %r8
        jnz     1b
        RET
SYM_FUNC_END(canvas_glyph)

#> arch x86_64
SYM_FUNC_START(canvas_cell)
        shl     $2, %rsi                # pitch, pixels to bytes

        #
        #       Sixteen entries of two pixel pairs, one for every nibble of
        #       the bitmap, built once for this cell's two colours.
        #
        #       The four pairs a nibble can be made of are paper-paper,
        #       paper-ink, ink-paper and ink-ink; entry n holds the pair its
        #       top two bits ask for and then the pair its bottom two ask
        #       for, so a row is two loads of sixteen bytes and four stores
        #       and no per bit work at all. The pixels within a pair are
        #       little endian -- the left one is the low half -- which is
        #       true of all three machines this file is written for.
        #
        #       The colours arrive as u32, so the top half of the register
        #       is whatever the caller last had there: cleared here rather
        #       than shifted into a pixel.
        #
        #       A sixteen row cell, call included, over a screen of the
        #       kernel's own boot log: 620 instructions became 345 here, 475
        #       became 243 on arm64 and 655 became 367 on riscv64. On a
        #       native Zen the same cell went from 110 cycles to 71.
        #
        #       Skipping the rows that are all paper was measured and is not
        #       here. They are 47 percent of the rows a boot log draws, but
        #       the table has already made such a row twelve instructions
        #       rather than thirty two, so the test costs about what it
        #       saves -- 346 against 345 -- and buys a branch that depends on
        #       the letter being drawn.
        #
        mov     %r9d, %eax              # paper
        mov     %r8d, %r8d              # ink
        mov     %rax, %r9
        shl     $32, %r9                # paper on the right
        mov     %r8, %r11
        shl     $32, %r11               # ink on the right
        mov     %rax, %r10
        or      %r11, %r10              # paper then ink
        or      %r8, %r11               # ink then ink
        or      %r9, %rax               # paper then paper
        or      %r9, %r8                # ink then paper

        sub     $256, %rsp
        mov     %rax, 0(%rsp)
        mov     %rax, 8(%rsp)
        mov     %rax, 16(%rsp)
        mov     %r10, 24(%rsp)
        mov     %rax, 32(%rsp)
        mov     %r8, 40(%rsp)
        mov     %rax, 48(%rsp)
        mov     %r11, 56(%rsp)
        mov     %r10, 64(%rsp)
        mov     %rax, 72(%rsp)
        mov     %r10, 80(%rsp)
        mov     %r10, 88(%rsp)
        mov     %r10, 96(%rsp)
        mov     %r8, 104(%rsp)
        mov     %r10, 112(%rsp)
        mov     %r11, 120(%rsp)
        mov     %r8, 128(%rsp)
        mov     %rax, 136(%rsp)
        mov     %r8, 144(%rsp)
        mov     %r10, 152(%rsp)
        mov     %r8, 160(%rsp)
        mov     %r8, 168(%rsp)
        mov     %r8, 176(%rsp)
        mov     %r11, 184(%rsp)
        mov     %r11, 192(%rsp)
        mov     %rax, 200(%rsp)
        mov     %r11, 208(%rsp)
        mov     %r10, 216(%rsp)
        mov     %r11, 224(%rsp)
        mov     %r8, 232(%rsp)
        mov     %r11, 240(%rsp)
        mov     %r11, 248(%rsp)

1:      movzbl  (%rdx), %eax
        inc     %rdx
        mov     %eax, %r10d
        and     $0xf0, %r10d            # the top nibble, times sixteen
        shl     $4, %eax
        and     $0xf0, %eax             # the bottom nibble, times sixteen

        mov     (%rsp,%r10,1), %r11
        mov     %r11, (%rdi)
        mov     8(%rsp,%r10,1), %r11
        mov     %r11, 8(%rdi)
        mov     (%rsp,%rax,1), %r11
        mov     %r11, 16(%rdi)
        mov     8(%rsp,%rax,1), %r11
        mov     %r11, 24(%rdi)

        add     %rsi, %rdi
        dec     %rcx
        jnz     1b

        add     $256, %rsp
        RET
SYM_FUNC_END(canvas_cell)

#> arch arm64
SYM_FUNC_START(canvas_cell)
        lsl     x1, x1, #2

        // The same sixteen entry table of pixel pairs; see the x86_64 block
        // above for what is in it. Here a row is two ldp and two stp.
        mov     w4, w4                  // ink, top half cleared
        mov     w5, w5                  // paper
        orr     x6, x5, x5, lsl #32     // paper then paper
        orr     x7, x5, x4, lsl #32     // paper then ink
        orr     x8, x4, x5, lsl #32     // ink then paper
        orr     x9, x4, x4, lsl #32     // ink then ink

        sub     sp, sp, #256
        stp     x6, x6, [sp, #0]
        stp     x6, x7, [sp, #16]
        stp     x6, x8, [sp, #32]
        stp     x6, x9, [sp, #48]
        stp     x7, x6, [sp, #64]
        stp     x7, x7, [sp, #80]
        stp     x7, x8, [sp, #96]
        stp     x7, x9, [sp, #112]
        stp     x8, x6, [sp, #128]
        stp     x8, x7, [sp, #144]
        stp     x8, x8, [sp, #160]
        stp     x8, x9, [sp, #176]
        stp     x9, x6, [sp, #192]
        stp     x9, x7, [sp, #208]
        stp     x9, x8, [sp, #224]
        stp     x9, x9, [sp, #240]

1:      ldrb    w6, [x2], #1
        and     x7, x6, #0xf0           // the top nibble, times sixteen
        ubfiz   x8, x6, #4, #4          // the bottom nibble, times sixteen
        add     x7, sp, x7
        add     x8, sp, x8
        ldp     x9, x10, [x7]
        ldp     x11, x12, [x8]
        stp     x9, x10, [x0]
        stp     x11, x12, [x0, #16]

        add     x0, x0, x1
        subs    x3, x3, #1
        b.ne    1b

        add     sp, sp, #256
        ret
SYM_FUNC_END(canvas_cell)

#> arch riscv64
SYM_FUNC_START(canvas_cell)
        slli    a1, a1, 2

        # The same sixteen entry table of pixel pairs; see the x86_64 block
        # above for what is in it.
        slli    a4, a4, 32
        srli    a4, a4, 32              # ink, top half cleared
        slli    a5, a5, 32
        srli    a5, a5, 32              # paper
        slli    t4, a5, 32              # paper on the right
        slli    t5, a4, 32              # ink on the right
        or      t0, a5, t4              # paper then paper
        or      t1, a5, t5              # paper then ink
        or      t2, a4, t4              # ink then paper
        or      t3, a4, t5              # ink then ink

        addi    sp, sp, -256
        sd      t0, 0(sp)
        sd      t0, 8(sp)
        sd      t0, 16(sp)
        sd      t1, 24(sp)
        sd      t0, 32(sp)
        sd      t2, 40(sp)
        sd      t0, 48(sp)
        sd      t3, 56(sp)
        sd      t1, 64(sp)
        sd      t0, 72(sp)
        sd      t1, 80(sp)
        sd      t1, 88(sp)
        sd      t1, 96(sp)
        sd      t2, 104(sp)
        sd      t1, 112(sp)
        sd      t3, 120(sp)
        sd      t2, 128(sp)
        sd      t0, 136(sp)
        sd      t2, 144(sp)
        sd      t1, 152(sp)
        sd      t2, 160(sp)
        sd      t2, 168(sp)
        sd      t2, 176(sp)
        sd      t3, 184(sp)
        sd      t3, 192(sp)
        sd      t0, 200(sp)
        sd      t3, 208(sp)
        sd      t1, 216(sp)
        sd      t3, 224(sp)
        sd      t2, 232(sp)
        sd      t3, 240(sp)
        sd      t3, 248(sp)

        # A pixel pointer is only aligned to four, and there is no Zbb and no
        # promise that a misaligned sd is anything but a trap into firmware.
        # Both the start of the cell and the step between its rows have to be
        # eight byte aligned for the pair path; an odd pitch puts every other
        # row half a pair out of step, so the pitch is in the test too.
        or      t4, a0, a1
        andi    t4, t4, 7
        bnez    t4, 3f

1:      lbu     a6, 0(a2)
        addi    a2, a2, 1
        andi    t4, a6, 0xf0            # the top nibble, times sixteen
        add     t4, sp, t4
        slli    t5, a6, 4
        andi    t5, t5, 0xf0            # the bottom nibble, times sixteen
        add     t5, sp, t5

        ld      t0, 0(t4)
        ld      t1, 8(t4)
        ld      t2, 0(t5)
        ld      t3, 8(t5)
        sd      t0, 0(a0)
        sd      t1, 8(a0)
        sd      t2, 16(a0)
        sd      t3, 24(a0)

        add     a0, a0, a1
        addi    a3, a3, -1
        bnez    a3, 1b

        addi    sp, sp, 256
        ret

        # A word at a time, which needs no alignment the caller has not
        # already given. The table is read as words out of the same entries.
3:      lbu     a6, 0(a2)
        addi    a2, a2, 1
        andi    t4, a6, 0xf0
        add     t4, sp, t4
        slli    t5, a6, 4
        andi    t5, t5, 0xf0
        add     t5, sp, t5

        lw      t0, 0(t4)
        lw      t1, 4(t4)
        lw      t2, 8(t4)
        lw      t3, 12(t4)
        sw      t0, 0(a0)
        sw      t1, 4(a0)
        sw      t2, 8(a0)
        sw      t3, 12(a0)
        lw      t0, 0(t5)
        lw      t1, 4(t5)
        lw      t2, 8(t5)
        lw      t3, 12(t5)
        sw      t0, 16(a0)
        sw      t1, 20(a0)
        sw      t2, 24(a0)
        sw      t3, 28(a0)

        add     a0, a0, a1
        addi    a3, a3, -1
        bnez    a3, 3b

        addi    sp, sp, 256
        ret
SYM_FUNC_END(canvas_cell)

#> arch other

#> shared


#> arch arm64
SYM_FUNC_START(canvas_glyph)
        lsl     x1, x1, #2

1:      ldrb    w6, [x2]
        add     x2, x2, x3
        cbz     w6, 2f                  // a blank row, and most rows are

        tbz     w6, #7, 3f
        str     w5, [x0]
3:      tbz     w6, #6, 4f
        str     w5, [x0, #4]
4:      tbz     w6, #5, 5f
        str     w5, [x0, #8]
5:      tbz     w6, #4, 6f
        str     w5, [x0, #12]
6:      tbz     w6, #3, 7f
        str     w5, [x0, #16]
7:      tbz     w6, #2, 8f
        str     w5, [x0, #20]
8:      tbz     w6, #1, 9f
        str     w5, [x0, #24]
9:      tbz     w6, #0, 2f
        str     w5, [x0, #28]

2:      add     x0, x0, x1
        subs    x4, x4, #1
        b.ne    1b
        ret
SYM_FUNC_END(canvas_glyph)

#> arch riscv64
SYM_FUNC_START(canvas_glyph)
        slli    a1, a1, 2

1:      lbu     a6, 0(a2)
        add     a2, a2, a3
        beqz    a6, 2f                  # a blank row, and most rows are

        andi    t0, a6, 0x80
        beqz    t0, 3f
        sw      a5, 0(a0)
3:      andi    t0, a6, 0x40
        beqz    t0, 4f
        sw      a5, 4(a0)
4:      andi    t0, a6, 0x20
        beqz    t0, 5f
        sw      a5, 8(a0)
5:      andi    t0, a6, 0x10
        beqz    t0, 6f
        sw      a5, 12(a0)
6:      andi    t0, a6, 0x08
        beqz    t0, 7f
        sw      a5, 16(a0)
7:      andi    t0, a6, 0x04
        beqz    t0, 8f
        sw      a5, 20(a0)
8:      andi    t0, a6, 0x02
        beqz    t0, 9f
        sw      a5, 24(a0)
9:      andi    t0, a6, 0x01
        beqz    t0, 2f
        sw      a5, 28(a0)

2:      add     a0, a0, a1
        addi    a4, a4, -1
        bnez    a4, 1b
        ret
SYM_FUNC_END(canvas_glyph)

#> arch other

#> shared
