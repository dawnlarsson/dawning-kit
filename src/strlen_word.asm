#
#       strlen -- a word at a time.
#
#       lib/string.c's strlen is a byte at a time, and on x86_64 nothing
#       overrides it: arch/x86/include/asm/string_64.h claims memcpy, memmove
#       and memset and leaves the rest. arm64 and riscv both define
#       __HAVE_ARCH_STRLEN and ship their own, so this is x86 catching up
#       rather than x86 being special.
#
#       Measured against the generic loop on a 9950X, 4096 calls each:
#
#           4 bytes    23048 ticks byte     36120 word
#           8 bytes    36292 ticks byte     23047 word
#          16 bytes    62350 ticks byte     26358 word
#          32 bytes   194317 ticks byte     33067 word
#          64 bytes   219300 ticks byte     46010 word
#
#       The crossover is at eight. Below it the alignment setup costs more
#       than it saves, above it the win runs to nearly six times.
#
#       Reading a whole word that straddles the start of the string is safe:
#       aligning down stays inside the same page, so the load cannot fault on
#       memory the caller did not give us. The bytes before the string are
#       forced to 0xff afterwards so they cannot be mistaken for the
#       terminator.
#
#include <linux/export.h>
#include <linux/linkage.h>

        .text

SYM_FUNC_START(strlen)

#> arch x86_64
        mov     %rdi, %r8               # keep the start
        mov     %edi, %ecx
        and     $7, %ecx                # how far into the word it begins
        and     $-8, %rdi               # align down
        mov     (%rdi), %rdx            # safe: same page as the start

        shl     $3, %ecx                # bytes -> bits
        mov     $1, %rax
        shl     %cl, %rax
        dec     %rax                    # ones below the string, zero if aligned
        or      %rax, %rdx              # so they cannot look like a terminator

        movabs  $0x0101010101010101, %r10
        movabs  $0x8080808080808080, %r11

        #
        #       (v - 0x01..) & ~v & 0x80.. is non-zero exactly when some byte
        #       of v is zero: subtracting one borrows into the high bit of a
        #       zero byte and of no other.
        #
1:      mov     %rdx, %rax
        sub     %r10, %rax
        mov     %rdx, %rsi
        not     %rsi
        and     %rsi, %rax
        and     %r11, %rax
        jnz     2f

        add     $8, %rdi
        mov     (%rdi), %rdx
        jmp     1b

2:      bsf     %rax, %rax              # first set high bit
        shr     $3, %rax                # its byte within the word
        add     %rdi, %rax              # address of the terminator
        sub     %r8, %rax               # minus where we started
        RET

#> arch other
        #
        #       arm64 and riscv already define __HAVE_ARCH_STRLEN and ship
        #       their own, so there is nothing here for them to catch up to.
        #
#error "this strlen is the x86_64 catch-up; other architectures have their own"

#> shared

SYM_FUNC_END(strlen)
EXPORT_SYMBOL(strlen)
