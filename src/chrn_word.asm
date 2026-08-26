//
//       strchrnul, strnchr and strrchr -- the rest of the byte hunts.
//
//       All three are strchr with one thing changed: what to return when the
//       byte is absent, where to stop, and which match to keep. The machinery
//       underneath is the one strchr already uses -- broadcast the byte across
//       a word, exclusive-or, and the byte that matched is the byte that is
//       now zero -- so what follows is mostly the differences.
//
#ifndef MOONWATER_FREESTANDING_ASM
#include <linux/export.h>
#include <linux/linkage.h>
#endif

        .text

//
//       char *strchrnul(const char *s, int c)
//
//       strchr that answers with the terminator instead of nothing. Since the
//       scan already stops at whichever of the two comes first, that is the
//       same code without the last test.
//

#> arch x86_64
SYM_FUNC_START(strchrnul)
        movzbl  %sil, %ecx
        movabs  $0x0101010101010101, %r10
        mov     %rcx, %rsi
        imul    %r10, %rsi
        movabs  $0x8080808080808080, %r11

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx

        shl     $3, %ecx
        mov     $-1, %r9
        shl     %cl, %r9

1:      mov     %rdx, %rax
        xor     %rsi, %rax
        mov     %rax, %rcx
        not     %rcx
        sub     %r10, %rax
        and     %rcx, %rax
        and     %r11, %rax

        mov     %rdx, %r8
        sub     %r10, %r8
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %r8
        and     %r11, %r8

        or      %r8, %rax
        and     %r9, %rax
        jnz     2f

        mov     $-1, %r9
        add     $8, %rdi
        mov     (%rdi), %rdx
        jmp     1b

2:      bsf     %rax, %rax
        shr     $3, %rax
        add     %rdi, %rax
        RET

SYM_FUNC_END(strchrnul)
EXPORT_SYMBOL(strchrnul)

#> arch arm64
SYM_FUNC_START(strchrnul)
        and     w1, w1, #0xff
        mov     x10, #0x0101
        movk    x10, #0x0101, lsl #16
        movk    x10, #0x0101, lsl #32
        movk    x10, #0x0101, lsl #48   // 0x0101010101010101
        lsl     x11, x10, #7            // 0x8080808080808080
        mul     x3, x1, x10             // the byte, in all eight positions

        and     x4, x0, #7              // how far into the word it begins
        bic     x5, x0, #7              // align down: same page, cannot fault
        ldr     x6, [x5]
        lsl     x4, x4, #3              // bytes -> bits
        mov     x7, #-1
        lsl     x7, x7, x4              // which bytes of the first word count

1:      eor     x8, x6, x3              // the byte that matched is now zero
        sub     x9, x8, x10
        bic     x9, x9, x8
        and     x9, x9, x11

        sub     x12, x6, x10            // and the terminator, the same way
        bic     x12, x12, x6
        and     x12, x12, x11

        orr     x9, x9, x12
        and     x9, x9, x7
        cbnz    x9, 2f

        mov     x7, #-1                 // every byte of every later word counts
        add     x5, x5, #8
        ldr     x6, [x5]
        b       1b

2:      rbit    x9, x9
        clz     x9, x9                  // first set high bit
        lsr     x9, x9, #3              // its byte within the word
        add     x0, x5, x9
        ret
SYM_FUNC_END(strchrnul)
EXPORT_SYMBOL(strchrnul)

#> arch riscv64
        //
        //      Where x86 has bsf and arm64 has rbit+clz, base rv64 has
        //      neither: ctz is Zbb, and QEMU's virt machine does not have it
        //      ("riscv: base ISA extensions acdfhim"), so requiring it would
        //      mean an illegal instruction on the machine this is developed
        //      on. So the byte index comes out of the multiply the M
        //      extension already guarantees:
        //
        //          x & -x                  keep only the lowest set bit
        //          - 1                     ones below it
        //          & 0x0101..01            one per byte below it
        //          * 0x0101..01 >> 56      count them, since each contributes
        //                                  1 to the top byte and there are at
        //                                  most eight
        //          - 1                     that count is the index plus one
        //
        //      Seven instructions where Zbb would take two, and only on the
        //      way out. The alternative was a floor that cannot be tested.
        //

SYM_FUNC_START(strchrnul)
        andi    a1, a1, 0xff
        li      t0, 0x0101010101010101
        slli    t1, t0, 7               // 0x8080808080808080
        mul     a3, a1, t0              // the byte, in all eight positions

        andi    a4, a0, 7               // how far into the word it begins
        andi    a5, a0, -8              // align down: same page, cannot fault
        ld      a6, 0(a5)
        slli    a4, a4, 3               // bytes -> bits
        li      a7, -1
        sll     a7, a7, a4              // which bytes of the first word count

1:      xor     t2, a6, a3              // the byte that matched is now zero
        sub     t3, t2, t0
        not     t4, t2
        and     t3, t3, t4
        and     t3, t3, t1

        sub     t5, a6, t0              // and the terminator, the same way
        not     t6, a6
        and     t5, t5, t6
        and     t5, t5, t1

        or      t3, t3, t5
        and     t3, t3, a7
        bnez    t3, 2f

        li      a7, -1                  // every byte of every later word counts
        addi    a5, a5, 8
        ld      a6, 0(a5)
        j       1b

2:      sub     t5, zero, t3
        and     t3, t3, t5              // lowest set high bit
        addi    t3, t3, -1
        and     t3, t3, t0
        mul     t3, t3, t0
        srli    t3, t3, 56
        addi    t3, t3, -1              // its byte within the word
        add     a0, a5, t3
        ret
SYM_FUNC_END(strchrnul)
EXPORT_SYMBOL(strchrnul)

#> arch other
        //
        //      Nothing, deliberately, and this is what "#> arch other" with an
        //      empty block is for: this file is in src/, so src/Makefile
        //      builds it for whichever architecture is being configured, and
        //      an architecture with no block of its own keeps the generic C
        //      in lib/string.c exactly as it had it.
        //

#> shared


//
//       char *strnchr(const char *s, size_t count, int c)
//
//       strchr with a fence. Note the argument order: the count comes second
//       and the byte third, which is the opposite way round from memchr and
//       is the kind of thing that is only wrong once.
//

#> arch x86_64
SYM_FUNC_START(strnchr)
        xor     %eax, %eax
        test    %rsi, %rsi
        jz      9f

        movzbl  %dl, %ecx
        movabs  $0x0101010101010101, %r10
        lea     (%rdi,%rsi), %r9        // one past the last byte we may report
        mov     %rcx, %rsi
        imul    %r10, %rsi
        movabs  $0x8080808080808080, %r11

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx

        shl     $3, %ecx
        //
        //      Into %r8, not %rcx: the shift count is in %cl, which is part of
        //      %rcx, so building the mask there destroys the count before the
        //      shift reads it. That mistake passed the build, booted, and was
        //      wrong in 293398 of 1401280 cases.
        //
        mov     $-1, %r8
        shl     %cl, %r8
        push    %r8                     // the valid-byte mask, out of registers

1:      mov     %rdx, %rax
        xor     %rsi, %rax
        mov     %rax, %rcx
        not     %rcx
        sub     %r10, %rax
        and     %rcx, %rax
        and     %r11, %rax

        mov     %rdx, %r8
        sub     %r10, %r8
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %r8
        and     %r11, %r8

        or      %r8, %rax
        and     (%rsp), %rax
        jnz     2f

        movq    $-1, (%rsp)
        add     $8, %rdi
        cmp     %r9, %rdi
        jae     8f
        mov     (%rdi), %rdx
        jmp     1b

2:      bsf     %rax, %rax
        shr     $3, %rax
        add     %rdi, %rax
        cmp     %r9, %rax
        jae     8f                      // beyond the count
        movzbl  (%rax), %ecx
        cmp     %sil, %cl
        je      3f

8:      xor     %eax, %eax
3:      add     $8, %rsp
9:      RET

SYM_FUNC_END(strnchr)
EXPORT_SYMBOL(strnchr)

#> arch arm64
SYM_FUNC_START(strnchr)
        mov     x9, #0
        cbz     x1, 9f

        and     w2, w2, #0xff
        mov     x10, #0x0101
        movk    x10, #0x0101, lsl #16
        movk    x10, #0x0101, lsl #32
        movk    x10, #0x0101, lsl #48
        lsl     x11, x10, #7
        mul     x3, x2, x10
        add     x13, x0, x1             // one past the last byte we may report

        and     x4, x0, #7
        bic     x5, x0, #7
        ldr     x6, [x5]
        lsl     x4, x4, #3
        mov     x7, #-1
        lsl     x7, x7, x4

1:      eor     x8, x6, x3
        sub     x9, x8, x10
        bic     x9, x9, x8
        and     x9, x9, x11

        sub     x12, x6, x10
        bic     x12, x12, x6
        and     x12, x12, x11

        orr     x9, x9, x12
        and     x9, x9, x7
        cbnz    x9, 2f

        mov     x7, #-1
        add     x5, x5, #8
        cmp     x5, x13
        b.hs    8f
        ldr     x6, [x5]
        b       1b

2:      rbit    x9, x9
        clz     x9, x9
        lsr     x9, x9, #3
        add     x9, x5, x9
        cmp     x9, x13
        b.hs    8f                      // beyond the count
        ldrb    w4, [x9]
        cmp     w4, w2
        b.eq    9f                      // it was the byte, not the terminator

8:      mov     x9, #0
9:      mov     x0, x9
        ret
SYM_FUNC_END(strnchr)
EXPORT_SYMBOL(strnchr)

#> arch riscv64
        //      The same count-the-bytes-below sequence as strchrnul above.
SYM_FUNC_START(strnchr)
        beqz    a1, 8f

        andi    a2, a2, 0xff
        li      t0, 0x0101010101010101
        slli    t1, t0, 7
        mul     a3, a2, t0
        add     a4, a0, a1              // one past the last byte we may report

        andi    t2, a0, 7
        andi    a5, a0, -8
        ld      a6, 0(a5)
        slli    t2, t2, 3
        li      a7, -1
        sll     a7, a7, t2

1:      xor     t3, a6, a3
        sub     t4, t3, t0
        not     t5, t3
        and     t4, t4, t5
        and     t4, t4, t1

        sub     t6, a6, t0
        not     t2, a6
        and     t6, t6, t2
        and     t6, t6, t1

        or      t4, t4, t6
        and     t4, t4, a7
        bnez    t4, 2f

        li      a7, -1
        addi    a5, a5, 8
        bgeu    a5, a4, 8f
        ld      a6, 0(a5)
        j       1b

2:      sub     t5, zero, t4
        and     t4, t4, t5              // lowest set high bit
        addi    t4, t4, -1
        and     t4, t4, t0
        mul     t4, t4, t0
        srli    t4, t4, 56
        addi    t4, t4, -1              // its byte within the word
        add     t4, a5, t4
        bgeu    t4, a4, 8f              // beyond the count
        lbu     t5, 0(t4)
        bne     t5, a2, 8f              // the terminator, not the byte
        mv      a0, t4
        ret

8:      li      a0, 0
        ret
SYM_FUNC_END(strnchr)
EXPORT_SYMBOL(strnchr)

#> arch other
        // As above: nothing here on purpose.

#> shared


//
//       char *strrchr(const char *s, int c)
//
//       The last match rather than the first, which the forward scan does not
//       answer directly: within a word the highest set bit is wanted, not the
//       lowest, so bsr where the others use bsf. A word without a terminator
//       may hold a later match than anything before it, so the best so far is
//       carried along; the word that holds the terminator only counts matches
//       below it.
//
//       Searching for the terminator itself is a byte walk. It is the one case
//       where the answer is the end of the string rather than a match inside
//       it, and it is rare enough not to be worth its own scan.
//

#> arch x86_64
SYM_FUNC_START(strrchr)
        movzbl  %sil, %ecx
        test    %cl, %cl
        jnz     4f

        // strrchr(s, 0) is the terminator.
1:      cmpb    $0, (%rdi)
        je      2f
        inc     %rdi
        jmp     1b
2:      mov     %rdi, %rax
        RET

4:      push    %rbx
        xor     %ebx, %ebx              // best so far: none

        movabs  $0x0101010101010101, %r10
        mov     %rcx, %rsi
        imul    %r10, %rsi
        movabs  $0x8080808080808080, %r11

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx

        shl     $3, %ecx
        mov     $-1, %r9
        shl     %cl, %r9

5:      mov     %rdx, %rax
        xor     %rsi, %rax
        mov     %rax, %rcx
        not     %rcx
        sub     %r10, %rax
        and     %rcx, %rax
        and     %r11, %rax
        and     %r9, %rax               // matches in this word

        mov     %rdx, %r8
        sub     %r10, %r8
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %r8
        and     %r11, %r8
        and     %r9, %r8                // terminator in this word

        test    %r8, %r8
        jnz     7f

        test    %rax, %rax
        jz      6f
        bsr     %rax, %rcx
        shr     $3, %rcx
        lea     (%rdi,%rcx), %rbx       // a later match than any before

6:      mov     $-1, %r9
        add     $8, %rdi
        mov     (%rdi), %rdx
        jmp     5b

        //
        //      The string ends in this word. Only matches below the
        //      terminator count: isolate its lowest bit and keep what is
        //      under it.
        //
7:      mov     %r8, %rcx
        neg     %rcx
        and     %r8, %rcx
        dec     %rcx
        and     %rcx, %rax
        jz      8f

        bsr     %rax, %rcx
        shr     $3, %rcx
        lea     (%rdi,%rcx), %rbx

8:      mov     %rbx, %rax
        pop     %rbx
        RET

SYM_FUNC_END(strrchr)
EXPORT_SYMBOL(strrchr)

#> arch other
        // As above: nothing here on purpose.

#> shared

