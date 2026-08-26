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

        .macro  cell_pixel bit, offset
        mov     %r9d, %r10d
        test    $\bit, %al
        cmovne  %r8d, %r10d
        mov     %r10d, \offset(%rdi)
        .endm

1:      movzbl  (%rdx), %eax
        inc     %rdx

        cell_pixel 0x80, 0
        cell_pixel 0x40, 4
        cell_pixel 0x20, 8
        cell_pixel 0x10, 12
        cell_pixel 0x08, 16
        cell_pixel 0x04, 20
        cell_pixel 0x02, 24
        cell_pixel 0x01, 28

        add     %rsi, %rdi
        dec     %rcx
        jnz     1b
        RET
SYM_FUNC_END(canvas_cell)

#> arch arm64
SYM_FUNC_START(canvas_cell)
        lsl     x1, x1, #2

        .macro  cell_pixel bit, offset
        tst     w6, #\bit
        csel    w7, w4, w5, ne
        str     w7, [x0, #\offset]
        .endm

1:      ldrb    w6, [x2], #1

        cell_pixel 0x80, 0
        cell_pixel 0x40, 4
        cell_pixel 0x20, 8
        cell_pixel 0x10, 12
        cell_pixel 0x08, 16
        cell_pixel 0x04, 20
        cell_pixel 0x02, 24
        cell_pixel 0x01, 28

        add     x0, x0, x1
        subs    x3, x3, #1
        b.ne    1b
        ret
SYM_FUNC_END(canvas_cell)

#> arch riscv64
SYM_FUNC_START(canvas_cell)
        slli    a1, a1, 2

        .macro  cell_pixel bit, offset
        mv      t0, a5
        andi    t1, a6, \bit
        beqz    t1, 8f
        mv      t0, a4
8:      sw      t0, \offset(a0)
        .endm

1:      lbu     a6, 0(a2)
        addi    a2, a2, 1

        cell_pixel 0x80, 0
        cell_pixel 0x40, 4
        cell_pixel 0x20, 8
        cell_pixel 0x10, 12
        cell_pixel 0x08, 16
        cell_pixel 0x04, 20
        cell_pixel 0x02, 24
        cell_pixel 0x01, 28

        add     a0, a0, a1
        addi    a3, a3, -1
        bnez    a3, 1b
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
        beqz    a6, 2f                  // a blank row, and most rows are

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
