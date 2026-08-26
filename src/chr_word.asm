#
#       strchr and memchr -- a word at a time.
#
#       The last two byte loops in lib/string.c worth taking. Both hunt for a
#       byte, so both broadcast it across a word and reuse the trick that finds
#       a zero byte: after exclusive-or with the broadcast, the byte that
#       matched is the byte that is now zero.
#
#       An eight byte load aligned to eight never crosses a page, so reading
#       the word that contains the string's first byte cannot fault on memory
#       the caller does not own. The matches in the bytes before the string are
#       thrown away afterwards by masking the result rather than the input,
#       which keeps the byte being searched for out of it -- forcing those
#       bytes to 0xff would false-match a search for 0xff.
#
#include <linux/export.h>
#include <linux/linkage.h>

        .text

#
#       char *strchr(const char *s, int c)
#
#       Two hunts at once: the byte, and the terminator that ends the search.
#       Whichever comes first in the word is the answer, and it is a hit only
#       if that byte is the one asked for -- which is also how strchr(s, 0)
#       returns the terminator rather than nothing.
#

#> arch x86_64
SYM_FUNC_START(strchr)
        movzbl  %sil, %ecx
        movabs  $0x0101010101010101, %r10
        mov     %rcx, %rsi
        imul    %r10, %rsi              # c in every byte; %sil is still c
        movabs  $0x8080808080808080, %r11

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx

        shl     $3, %ecx
        mov     $-1, %r9
        shl     %cl, %r9                # which bytes of this word are ours

1:      mov     %rdx, %rax
        xor     %rsi, %rax              # zero where the byte matched
        mov     %rax, %rcx
        not     %rcx
        sub     %r10, %rax
        and     %rcx, %rax
        and     %r11, %rax              # found the byte

        mov     %rdx, %r8
        sub     %r10, %r8
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %r8
        and     %r11, %r8               # found the terminator

        or      %r8, %rax
        and     %r9, %rax
        jnz     2f

        mov     $-1, %r9                # past the first word, all of it is ours
        add     $8, %rdi
        mov     (%rdi), %rdx
        jmp     1b

2:      bsf     %rax, %rax
        shr     $3, %rax
        add     %rdi, %rax              # the byte or the terminator, whichever
        movzbl  (%rax), %ecx
        cmp     %sil, %cl
        je      3f
        xor     %eax, %eax              # it was the terminator: not found
3:      RET

SYM_FUNC_END(strchr)
EXPORT_SYMBOL(strchr)

#> arch other
//
        //      Nothing, deliberately, and this is what "#> arch other" with an
        //      empty block is for.
        //
        //      This file is in src/, so src/Makefile builds it for whichever
        //      architecture the kernel is being configured for. An #error here
        //      -- which is what stood in this place -- does not mean "not
        //      implemented", it means the arm64 and riscv builds stop on the
        //      first of these files they reach.
        //
        //      They do not need it. arm64 and riscv both ship their own, and
        //      where they do not the generic C in lib/string.c is what runs,
        //      exactly as it did before any of this. Emitting nothing leaves
        //      them where they were; the header claim that hands the symbol
        //      over is in build.sh and only ever touches x86's header.
        //

#> shared


#
#       void *memchr(const void *s, int c, size_t n)
#
#       The same hunt with a fence instead of a terminator. A match found in
#       the word that reaches past the bound is discarded by comparing its
#       address, which is cheaper than stopping the scan short.
#

#> arch x86_64
SYM_FUNC_START(memchr)
        xor     %eax, %eax
        test    %rdx, %rdx
        jz      9f

        movzbl  %sil, %ecx
        movabs  $0x0101010101010101, %r10
        mov     %rcx, %rsi
        imul    %r10, %rsi
        movabs  $0x8080808080808080, %r11

        lea     (%rdi,%rdx), %r9        # one past the last byte we may report

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx
        xor     %rsi, %rdx

        shl     $3, %ecx
        mov     $-1, %r8
        shl     %cl, %r8

1:      mov     %rdx, %rax
        sub     %r10, %rax
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %rax
        and     %r11, %rax
        and     %r8, %rax
        jnz     2f

        mov     $-1, %r8
        add     $8, %rdi
        cmp     %r9, %rdi
        jae     8f
        mov     (%rdi), %rdx
        xor     %rsi, %rdx
        jmp     1b

2:      bsf     %rax, %rax
        shr     $3, %rax
        add     %rdi, %rax
        cmp     %r9, %rax
        jb      9f                      # inside the bound: that is the answer

8:      xor     %eax, %eax
9:      RET

SYM_FUNC_END(memchr)
EXPORT_SYMBOL(memchr)

#> arch riscv64
        //
        //      arm64 claims memchr and ships its own; riscv does not, so this
        //      is the one architecture of the three that still runs the byte
        //      loop here. ctz is Zbb, which RVA22 requires.
        //
        .option arch, +zbb

SYM_FUNC_START(memchr)
        beqz    a2, 8f

        andi    a1, a1, 0xff
        li      t0, 0x0101010101010101
        slli    t1, t0, 7
        mul     a3, a1, t0              // the byte, in all eight positions
        add     a4, a0, a2              // one past the last byte we may report

        andi    t2, a0, 7               // how far into the word it begins
        andi    a5, a0, -8              // align down: same page, cannot fault
        ld      a6, 0(a5)
        xor     a6, a6, a3              // the byte that matched is now zero
        slli    t2, t2, 3
        li      a7, -1
        sll     a7, a7, t2              // which bytes of the first word count

1:      sub     t3, a6, t0
        not     t4, a6
        and     t3, t3, t4
        and     t3, t3, t1
        and     t3, t3, a7
        bnez    t3, 2f

        li      a7, -1
        addi    a5, a5, 8
        bgeu    a5, a4, 8f
        ld      a6, 0(a5)
        xor     a6, a6, a3
        j       1b

2:      ctz     t3, t3
        srli    t3, t3, 3
        add     t3, a5, t3
        bgeu    t3, a4, 8f              // the word ran past the bound
        mv      a0, t3
        ret

8:      li      a0, 0
        ret
SYM_FUNC_END(memchr)
EXPORT_SYMBOL(memchr)

#> arch other
        // As above: nothing here on purpose.

#> shared

