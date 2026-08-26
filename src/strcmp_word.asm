//
//       strcmp -- a word at a time, with two pointers and no length.
//
//       The hard one, and the reason is worth stating. strlen could align its
//       pointer down, because an eight byte read aligned to eight never leaves
//       the page it started in. strncmp and memcmp could read unaligned,
//       because a length told them how far they were allowed to go. strcmp has
//       neither: two pointers at whatever alignments the caller chose, and
//       nothing but a terminator to say where they end. Aligning one down
//       misaligns the other, and reading unaligned past the terminator can
//       walk into a page nobody mapped.
//
//       What makes it safe is the same fact stated differently: a read of
//       eight bytes cannot fault if all eight are in a page that already holds
//       a byte we are allowed to read. The byte at the pointer is such a byte
//       -- the string has not ended yet, or we would have stopped -- so the
//       read is safe whenever it does not cross the page boundary, and the
//       offset within the page says whether it does.
//
//       Two compares buy eight bytes of progress. Near a page boundary the
//       loop steps a byte at a time until it is past, which happens for at
//       most seven bytes out of every four thousand and ninety six.
//
#include <linux/export.h>
#include <linux/linkage.h>

        .text


#> arch x86_64
SYM_FUNC_START(strcmp)
        movabs  $0x0101010101010101, %r10
        movabs  $0x8080808080808080, %r11

1:      //
        //      Would either read cross a page? 0xff8 is the last offset at
        //      which eight bytes still fit.
        //
        mov     %edi, %ecx
        and     $0xfff, %ecx
        cmp     $0xff8, %ecx
        ja      2f

        mov     %esi, %ecx
        and     $0xfff, %ecx
        cmp     $0xff8, %ecx
        ja      2f

        mov     (%rdi), %r8
        mov     (%rsi), %r9
        cmp     %r8, %r9
        jne     2f                      // differ: let the byte step find where

        //
        //      Eight equal bytes. If a terminator is among them the strings
        //      ended together and are equal.
        //
        mov     %r8, %rax
        sub     %r10, %rax
        mov     %r8, %rcx
        not     %rcx
        and     %rcx, %rax
        and     %r11, %rax
        jnz     3f

        add     $8, %rdi
        add     $8, %rsi
        jmp     1b

        //
        //      One byte, then back to the word loop. Reached when a read would
        //      cross a page and when a word differs -- in the second case it
        //      walks the few bytes to the difference, which happens once.
        //
2:      movzbl  (%rdi), %eax
        movzbl  (%rsi), %ecx
        sub     %ecx, %eax
        jnz     4f
        test    %ecx, %ecx
        jz      3f

        inc     %rdi
        inc     %rsi
        jmp     1b

3:      xor     %eax, %eax
4:      RET

SYM_FUNC_END(strcmp)
EXPORT_SYMBOL(strcmp)

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

