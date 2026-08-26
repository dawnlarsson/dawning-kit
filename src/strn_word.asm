#
#       strncmp and strnlen -- a word at a time.
#
#       Both are byte loops in lib/string.c and neither is overridden on
#       x86_64. Two functions in one file, which the dialect allows now: a
#       #> shared closes the run of blocks before it and the next #> arch
#       opens a new one.
#
#       An eight byte load aligned to eight never crosses a page, so reading
#       the word that contains a pointer can never fault on memory the caller
#       did not give us. That is what makes the unbounded scan safe; where a
#       length is given the reads are bounded anyway.
#
#include <linux/export.h>
#include <linux/linkage.h>

        .text

#
#       int strncmp(const char *a, const char *b, size_t n)
#
#       Eight bytes from each, unaligned, which is legal here because n bounds
#       the read. Equal and no terminator in them means advance; anything else
#       hands those eight to the byte loop, which already knows how to stop on
#       a difference or a terminator and gets the sign right. The difference is
#       found once per call, so simple beats clever there.
#

#> arch x86_64
SYM_FUNC_START(strncmp)
        xor     %eax, %eax
        test    %rdx, %rdx
        jz      9f

        movabs  $0x0101010101010101, %r10
        movabs  $0x8080808080808080, %r11

1:      cmp     $8, %rdx
        jb      2f

        mov     (%rdi), %r8
        mov     (%rsi), %r9
        cmp     %r8, %r9
        jne     2f                      # let the byte loop settle it

        # Equal so far. If either holds a terminator the strings end here.
        mov     %r8, %rax
        sub     %r10, %rax
        mov     %r8, %rcx
        not     %rcx
        and     %rcx, %rax
        and     %r11, %rax
        jnz     8f

        add     $8, %rdi
        add     $8, %rsi
        sub     $8, %rdx
        jmp     1b

2:      test    %rdx, %rdx
        jz      8f

3:      movzbl  (%rdi), %eax
        movzbl  (%rsi), %ecx
        sub     %ecx, %eax
        jnz     9f
        test    %ecx, %ecx
        jz      8f

        inc     %rdi
        inc     %rsi
        dec     %rdx
        jnz     3b

8:      xor     %eax, %eax
9:      RET

SYM_FUNC_END(strncmp)
EXPORT_SYMBOL(strncmp)

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
#       size_t strnlen(const char *s, size_t n)
#
#       strlen with a fence. The scan is the same -- align down, force the
#       bytes before the string non-zero, then look for a zero byte eight at a
#       time -- and a terminator found beyond n is clamped back to n, which is
#       what strnlen returns when there is none inside the bound.
#

#> arch x86_64
SYM_FUNC_START(strnlen)
        xor     %eax, %eax
        test    %rsi, %rsi
        jz      9f

        mov     %rdi, %r8               # start
        lea     (%rdi,%rsi), %r9        # one past the last byte we may report

        mov     %edi, %ecx
        and     $7, %ecx
        and     $-8, %rdi
        mov     (%rdi), %rdx

        shl     $3, %ecx
        mov     $1, %rax
        shl     %cl, %rax
        dec     %rax
        or      %rax, %rdx

        movabs  $0x0101010101010101, %r10
        movabs  $0x8080808080808080, %r11

1:      mov     %rdx, %rax
        sub     %r10, %rax
        mov     %rdx, %rcx
        not     %rcx
        and     %rcx, %rax
        and     %r11, %rax
        jnz     2f

        add     $8, %rdi
        cmp     %r9, %rdi
        jae     3f                      # nothing within the bound
        mov     (%rdi), %rdx
        jmp     1b

2:      bsf     %rax, %rax
        shr     $3, %rax
        add     %rdi, %rax              # where the terminator is
        sub     %r8, %rax               # how far in that is
        cmp     %rsi, %rax
        cmova   %rsi, %rax              # never more than n
        RET

3:      mov     %rsi, %rax
9:      RET

SYM_FUNC_END(strnlen)
EXPORT_SYMBOL(strnlen)

#> arch other
        // As above: nothing here on purpose.

#> shared

