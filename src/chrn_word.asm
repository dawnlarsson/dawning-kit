//
//       strchrnul, strnchr and strrchr -- the rest of the byte hunts.
//
//       All three are strchr with one thing changed: what to return when the
//       byte is absent, where to stop, and which match to keep. The machinery
//       underneath is the one strchr already uses -- broadcast the byte across
//       a word, exclusive-or, and the byte that matched is the byte that is
//       now zero -- so what follows is mostly the differences.
//
#include <linux/export.h>
#include <linux/linkage.h>

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

