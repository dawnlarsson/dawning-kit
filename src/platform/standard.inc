/*
        Experimental C standard library

        The small routines a C program expects to find, floored

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_STANDARD
#define STANDARD_MODERN_C_PLATFORM_STANDARD

/*
        Classifying a byte, and the two that change its case.

        These are the ones a C library spends its time in and the ones every
        implementation writes as a table lookup: a 256-entry array of flag
        bits, indexed by the character, one load and one test. That is a
        cache line touched to answer a question about a range.

        Every one of them below is arithmetic instead, and branchless. A range
        test is a subtract and an unsigned compare -- c - '0' below ten is
        exactly "is a digit", because a byte under '0' wraps to an enormous
        unsigned number and fails the same compare that a byte over '9' fails.
        One subtract, one compare, one set. No memory at all.

        The case pair is the same trick with the answer used as a number
        rather than a result: the comparison produces one or zero, shifting it
        five places makes it thirty two or zero, and adding or subtracting
        that is the whole of the conversion. No branch, so nothing to predict
        wrongly on text that alternates.

        The argument is an int, and may be EOF, which is -1. Subtracting and
        comparing unsigned handles that with nothing written down: -1 - '0'
        is a very large number and fails every range here.
*/

b32 byte_is_digit(b32 value);
b32 byte_is_upper(b32 value);
b32 byte_is_lower(b32 value);
b32 byte_is_alpha(b32 value);
b32 byte_is_alnum(b32 value);
b32 byte_is_space(b32 value);
b32 byte_is_hexadecimal(b32 value);
b32 byte_to_upper(b32 value);
b32 byte_to_lower(b32 value);

/*
        Absolute value, without a branch and without a comparison.

        The sign of a two's complement number is its top bit repeated, which
        an arithmetic shift produces directly: all ones for a negative, all
        zeros otherwise. Exclusive-or with that flips every bit of a negative
        and leaves a positive alone, and subtracting it adds the one that
        turns a flip into a negation. Three instructions, no branch, so a
        stream of alternating signs costs what a stream of one sign costs.

        Two widths because the C library has two and they are not the same
        function: abs takes an int and labs a long, and reading a 64 bit
        register when an int was passed reads whatever happened to be in the
        top half of it.
*/
b32 absolute_whole(b32 value);
bipolar absolute_wide(bipolar value);

/*
        Square root, which is one instruction everywhere this runs.

        x86-64 has had sqrtsd since SSE2, which is in the baseline; arm64 has
        fsqrt; and riscv has fsqrt.d in the D extension, which this project
        builds for. So the floor here is not an approximation to be tuned but
        the instruction the hardware already has, and a library that called
        anything else would be slower and less accurate at once.

        The absolute value is not quite one instruction on x86, which has no
        scalar fabs: clearing the sign bit through a general register is three
        moves and beats loading a mask constant from memory to and with.

        These are the only two here that take a decimal, and a decimal in a
        signature is refused by the arm64 kernel build whether or not anything
        calls it, so they and their bodies stay out of a kernel build entirely
        -- which is correct anyway, since kernel code may not touch the
        floating point registers without asking first.
*/
#ifndef KERNEL_MODE
decimal square_root(decimal value);
decimal absolute(decimal value);
#endif

#if X64
__asm__(
    ASM_SECTION

    ASM_FUNC(byte_is_digit)
    "lea -48(%rdi), %eax\n   cmp $10, %eax\n   setb %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "lea -65(%rdi), %eax\n   cmp $26, %eax\n   setb %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "lea -97(%rdi), %eax\n   cmp $26, %eax\n   setb %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_lower)

    //
    //       Setting bit five folds the upper case range onto the lower one,
    //       so one range test answers for both.
    //
    ASM_FUNC(byte_is_alpha)
    "mov %edi, %eax\n   or $32, %eax\n   sub $97, %eax\n"
    "cmp $26, %eax\n   setb %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    ASM_FUNC(byte_is_alnum)
    "mov %edi, %eax\n   or $32, %eax\n   sub $97, %eax\n"
    "cmp $26, %eax\n   setb %dl\n"
    "lea -48(%rdi), %eax\n   cmp $10, %eax\n   setb %al\n"
    "or %dl, %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    //
    //       Tab through carriage return are five in a row, so they are one
    //       range, and the space is the only one left over.
    //
    ASM_FUNC(byte_is_space)
    "lea -9(%rdi), %eax\n   cmp $5, %eax\n   setb %dl\n"
    "cmp $32, %edi\n   sete %al\n   or %dl, %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "lea -48(%rdi), %eax\n   cmp $10, %eax\n   setb %dl\n"
    "mov %edi, %eax\n   or $32, %eax\n   sub $97, %eax\n"
    "cmp $6, %eax\n   setb %al\n   or %dl, %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    //
    //       The comparison is the arithmetic: one or zero, shifted to thirty
    //       two or zero, taken off or put on.
    //
    ASM_FUNC(byte_to_upper)
    "lea -97(%rdi), %eax\n   cmp $26, %eax\n   setb %al\n   movzbl %al, %eax\n"
    "shl $5, %eax\n   mov %edi, %edx\n   sub %eax, %edx\n   mov %edx, %eax\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "lea -65(%rdi), %eax\n   cmp $26, %eax\n   setb %al\n   movzbl %al, %eax\n"
    "shl $5, %eax\n   add %edi, %eax\n"
    ASM_RET
    ASM_END(byte_to_lower)

    ASM_FUNC(absolute_whole)
    "mov %edi, %eax\n   mov %edi, %edx\n   sar $31, %edx\n"
    "xor %edx, %eax\n   sub %edx, %eax\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "mov %rdi, %rax\n   mov %rdi, %rdx\n   sar $63, %rdx\n"
    "xor %rdx, %rax\n   sub %rdx, %rax\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "sqrtsd %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(square_root)

    //
    //       No scalar fabs here, so the sign bit is cleared where the bits
    //       can be reached: through a general register, which is cheaper than
    //       loading a mask from memory to and with.
    //
    ASM_FUNC(absolute)
    "movq %xmm0, %rax\n   btr $63, %rax\n   movq %rax, %xmm0\n"
    ASM_RET
    ASM_END(absolute)
#endif
);
#elif ARM64
__asm__(
    ASM_SECTION

    ASM_FUNC(byte_is_digit)
    "sub w1, w0, #48\n   cmp w1, #10\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "sub w1, w0, #65\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "sub w1, w0, #97\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_lower)

    ASM_FUNC(byte_is_alpha)
    "orr w1, w0, #32\n   sub w1, w1, #97\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    ASM_FUNC(byte_is_alnum)
    "orr w1, w0, #32\n   sub w1, w1, #97\n   cmp w1, #26\n   cset w1, lo\n"
    "sub w2, w0, #48\n   cmp w2, #10\n   cset w2, lo\n   orr w0, w1, w2\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    ASM_FUNC(byte_is_space)
    "sub w1, w0, #9\n   cmp w1, #5\n   cset w1, lo\n"
    "cmp w0, #32\n   cset w2, eq\n   orr w0, w1, w2\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "sub w1, w0, #48\n   cmp w1, #10\n   cset w1, lo\n"
    "orr w2, w0, #32\n   sub w2, w2, #97\n   cmp w2, #6\n   cset w2, lo\n"
    "orr w0, w1, w2\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    ASM_FUNC(byte_to_upper)
    "sub w1, w0, #97\n   cmp w1, #26\n   cset w1, lo\n"
    "lsl w1, w1, #5\n   sub w0, w0, w1\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "sub w1, w0, #65\n   cmp w1, #26\n   cset w1, lo\n"
    "lsl w1, w1, #5\n   add w0, w0, w1\n"
    ASM_RET
    ASM_END(byte_to_lower)

    //
    //       cneg is the whole of it here: negate when the flags say minus.
    //
    ASM_FUNC(absolute_whole)
    "cmp w0, #0\n   cneg w0, w0, mi\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "cmp x0, #0\n   cneg x0, x0, mi\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "fsqrt d0, d0\n"
    ASM_RET
    ASM_END(square_root)

    ASM_FUNC(absolute)
    "fabs d0, d0\n"
    ASM_RET
    ASM_END(absolute)
#endif
);
#elif RISCV64
__asm__(
    ASM_SECTION

    ASM_FUNC(byte_is_digit)
    "addi t0, a0, -48\n   sltiu a0, t0, 10\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "addi t0, a0, -65\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "addi t0, a0, -97\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_lower)

    ASM_FUNC(byte_is_alpha)
    "ori t0, a0, 32\n   addi t0, t0, -97\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    ASM_FUNC(byte_is_alnum)
    "ori t0, a0, 32\n   addi t0, t0, -97\n   sltiu t0, t0, 26\n"
    "addi t1, a0, -48\n   sltiu t1, t1, 10\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    ASM_FUNC(byte_is_space)
    "addi t0, a0, -9\n   sltiu t0, t0, 5\n"
    "addi t1, a0, -32\n   seqz t1, t1\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "addi t0, a0, -48\n   sltiu t0, t0, 10\n"
    "ori t1, a0, 32\n   addi t1, t1, -97\n   sltiu t1, t1, 6\n"
    "or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    ASM_FUNC(byte_to_upper)
    "addi t0, a0, -97\n   sltiu t0, t0, 26\n   slli t0, t0, 5\n   sub a0, a0, t0\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "addi t0, a0, -65\n   sltiu t0, t0, 26\n   slli t0, t0, 5\n   add a0, a0, t0\n"
    ASM_RET
    ASM_END(byte_to_lower)

    //
    //       sraiw for the narrow one, so the sign that is repeated is the
    //       one at bit 31 rather than bit 63.
    //
    ASM_FUNC(absolute_whole)
    "sraiw t0, a0, 31\n   xor a0, a0, t0\n   subw a0, a0, t0\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "srai t0, a0, 63\n   xor a0, a0, t0\n   sub a0, a0, t0\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "fsqrt.d fa0, fa0\n"
    ASM_RET
    ASM_END(square_root)

    ASM_FUNC(absolute)
    "fabs.d fa0, fa0\n"
    ASM_RET
    ASM_END(absolute)
#endif
);
#endif

/*
        And the names a C program knows them by.

        Out of a kernel build, where the kernel writes its own isdigit and
        two definitions of it is one too many.
*/
#ifndef KERNEL_MODE
__asm__(
    ASM_ALIAS(isdigit,  byte_is_digit)
    ASM_ALIAS(isupper,  byte_is_upper)
    ASM_ALIAS(islower,  byte_is_lower)
    ASM_ALIAS(isalpha,  byte_is_alpha)
    ASM_ALIAS(isalnum,  byte_is_alnum)
    ASM_ALIAS(isspace,  byte_is_space)
    ASM_ALIAS(isxdigit, byte_is_hexadecimal)
    ASM_ALIAS(toupper,  byte_to_upper)
    ASM_ALIAS(tolower,  byte_to_lower)
    ASM_ALIAS(sqrt,     square_root)
    ASM_ALIAS(fabs,     absolute)
    ASM_ALIAS(abs,      absolute_whole)
    ASM_ALIAS(labs,     absolute_wide)
    ASM_ALIAS(llabs,    absolute_wide)
);
#endif

#endif // STANDARD_MODERN_C_PLATFORM_STANDARD
