/*
        lex_line_floor -- one line into tokens, without calling string_span.

        The C loop was two spans a token (blanks, then the word) plus a call
        to classify the operator. Most of those spans look at one byte and
        return. The bodies below keep string_span's membership load, but the
        first miss is the classify of that byte rather than a return to C.

        Ordinary words and operators stay here. Quotes, substitutions, [[, ((
        and a=(...) jump to the C walks that already own those contracts.
        A following '(' is never a simple terminator: that is a=(...) and
        extglob, which lex_word decides.

        Private store ABI, live across the helper:
            arm64  x24=lex_context x26=tokens w27=count
            x86_64 r14=lex_context r15=tokens ebx=count
            riscv  s2=lex_context  s8=tokens  s6=count
        Arguments: kind, op, text, length, at.
*/

#if X64
__asm__(
    ASM_SECTION
    ASM_LOCAL_FUNC(lex_line_store)
    ASM_ENDBR
    "mov 8(%r14), %rax\n   lea 2(%rbx), %r11\n   cmp %rax, %r11\n   jbe 1f\n"
    "push %rdi\n   push %rsi\n   push %rdx\n   push %rcx\n   push %r8\n"
    "call lex_grow\n   pop %r8\n   pop %rcx\n   pop %rdx\n   pop %rsi\n"
    "pop %rdi\n   test %eax, %eax\n   jz 2f\n   mov (%r14), %r15\n"
    "1:  mov %rbx, %rax\n   shl $5, %rax\n   add %r15, %rax\n   mov %edi, (%rax)\n"
    "mov %esi, 4(%rax)\n   mov %rdx, 8(%rax)\n   mov %rcx, 16(%rax)\n"
    "mov %r8, 24(%rax)\n   inc %ebx\n   mov %ebx, 16(%r14)\n   mov $1, %eax\n"
    ASM_RET
    "2:  xor %eax, %eax\n"
    ASM_RET
    ASM_LOCAL_END(lex_line_store)

    ASM_FUNC(lex_line_floor)
    "push %rbp\n   mov %rsp, %rbp\n   push %rbx\n   push %r12\n   push %r13\n"
    "push %r14\n   push %r15\n   sub $24, %rsp\n"
    "mov %rdi, %r12\n   mov %esi, 8(%rsp)\n   lea lex_context(%rip), %r14\n"
    "xor %ebx, %ebx\n   mov %ebx, 16(%r14)\n   mov (%r14), %r15\n   mov %r12, %r13\n"

    ".Llex_x64_loop:\n"
    "lea string_set_blanks(%rip), %rax\n"
    "3:  movzbl (%r13), %ecx\n   cmpb $0, (%rax,%rcx)\n   je 4f\n   inc %r13\n   jmp 3b\n"
    "4:  test %ecx, %ecx\n   jz .Llex_x64_done\n   cmp $10, %ecx\n   je .Llex_x64_done\n"
    "cmp $35, %ecx\n   jne 5f\n   cmpl $0, 8(%rsp)\n   jne .Llex_x64_done\n"

    "5:  mov %r13, %r8\n   sub %r12, %r8\n   mov %r8, (%rsp)\n   lea lex_at(%rip), %rax\n"
    "mov %r8, (%rax)\n"

    "cmp $91, %ecx\n   jne 6f\n   cmpb $91, 1(%r13)\n   jne 6f\n"
    "movzbl 2(%r13), %eax\n   cmp $32, %eax\n   je 7f\n   cmp $9, %eax\n   je 7f\n"
    "cmp $10, %eax\n   jne 6f\n"
    "7:  mov %ebx, 16(%r14)\n   mov %r13, %rdi\n   call lex_conditional_end\n"
    "test %rax, %rax\n   jz 6f\n   mov %rax, %r9\n   sub %r13, %r9\n   mov %r9, 16(%rsp)\n"
    "mov $4, %edi\n   xor %esi, %esi\n   mov %r13, %rdx\n   mov %r9, %rcx\n"
    "mov (%rsp), %r8\n   call lex_line_store\n   test %eax, %eax\n   jz .Llex_x64_fail\n"
    "add 16(%rsp), %r13\n   jmp .Llex_x64_loop\n"

    "6:  movzbl (%r13), %ecx\n   cmp $40, %ecx\n   jne 8f\n   cmpb $40, 1(%r13)\n   jne 8f\n"
    "mov %ebx, 16(%r14)\n   mov %r13, %rdi\n   call lex_arithmetic_end\n"
    "test %rax, %rax\n   jz 8f\n   mov %rax, %r9\n   sub %r13, %r9\n   mov %r9, 16(%rsp)\n"
    "mov $3, %edi\n   xor %esi, %esi\n   mov %r13, %rdx\n   mov %r9, %rcx\n"
    "mov (%rsp), %r8\n   call lex_line_store\n   test %eax, %eax\n   jz .Llex_x64_fail\n"
    "add 16(%rsp), %r13\n   jmp .Llex_x64_loop\n"

    "8:  movzbl (%r13), %ecx\n   lea lex_operator(%rip), %rax\n   cmpb $0, (%rax,%rcx)\n"
    "je .Llex_x64_word\n   movzbl 1(%r13), %edx\n   movzbl 2(%r13), %r9d\n"
    "cmp $40, %edx\n   jne 9f\n   cmp $60, %ecx\n   je .Llex_x64_word\n"
    "cmp $62, %ecx\n   je .Llex_x64_word\n"

    "9:  xor %esi, %esi\n   mov $1, %r10d\n"
    "cmp $60, %ecx\n   je .Llex_x64_lt\n   cmp $62, %ecx\n   je .Llex_x64_gt\n"
    "cmp $38, %ecx\n   je .Llex_x64_amp\n   cmp $124, %ecx\n   je .Llex_x64_pipe\n"
    "cmp $59, %ecx\n   je .Llex_x64_semi\n   cmp $40, %ecx\n   je .Llex_x64_lpar\n"
    "cmp $41, %ecx\n   je .Llex_x64_rpar\n   jmp .Llex_x64_word\n"

    ".Llex_x64_lt:\n"
    "cmp $60, %edx\n   jne 10f\n   cmp $60, %r9d\n   jne 11f\n"
    "mov $19, %esi\n   mov $3, %r10d\n   jmp .Llex_x64_op\n"
    "11: mov $4, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "10: cmp $38, %edx\n   jne 12f\n   mov $6, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "12: cmp $62, %edx\n   jne .Llex_x64_lt1\n   mov $8, %esi\n   mov $2, %r10d\n"
    "jmp .Llex_x64_op\n"
    ".Llex_x64_lt1:\n   mov $13, %esi\n   jmp .Llex_x64_op\n"

    ".Llex_x64_gt:\n"
    "cmp $62, %edx\n   jne 13f\n   mov $5, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "13: cmp $38, %edx\n   jne 14f\n   mov $7, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "14: cmp $124, %edx\n   jne .Llex_x64_gt1\n   mov $9, %esi\n   mov $2, %r10d\n"
    "jmp .Llex_x64_op\n"
    ".Llex_x64_gt1:\n   mov $14, %esi\n   jmp .Llex_x64_op\n"

    ".Llex_x64_amp:\n"
    "cmp $62, %edx\n   jne 15f\n   cmp $62, %r9d\n   jne 16f\n"
    "mov $18, %esi\n   mov $3, %r10d\n   jmp .Llex_x64_op\n"
    "16: mov $17, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "15: cmp $38, %edx\n   jne .Llex_x64_amp1\n   mov $1, %esi\n   mov $2, %r10d\n"
    "jmp .Llex_x64_op\n"
    ".Llex_x64_amp1:\n   mov $12, %esi\n   jmp .Llex_x64_op\n"

    ".Llex_x64_pipe:\n"
    "cmp $124, %edx\n   jne 17f\n   mov $2, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "17: cmp $38, %edx\n   jne .Llex_x64_pipe1\n   mov $20, %esi\n   mov $2, %r10d\n"
    "jmp .Llex_x64_op\n"
    ".Llex_x64_pipe1:\n   mov $11, %esi\n   jmp .Llex_x64_op\n"

    ".Llex_x64_semi:\n"
    "cmp $38, %edx\n   jne 18f\n   mov $21, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    "18: cmp $59, %edx\n   jne .Llex_x64_semi1\n   cmp $38, %r9d\n   jne 19f\n"
    "mov $22, %esi\n   mov $3, %r10d\n   jmp .Llex_x64_op\n"
    "19: mov $3, %esi\n   mov $2, %r10d\n   jmp .Llex_x64_op\n"
    ".Llex_x64_semi1:\n   mov $10, %esi\n   jmp .Llex_x64_op\n"

    ".Llex_x64_lpar:\n   mov $15, %esi\n   jmp .Llex_x64_op\n"
    ".Llex_x64_rpar:\n   mov $16, %esi\n"

    ".Llex_x64_op:\n"
    "test %esi, %esi\n   jz .Llex_x64_word\n   mov %r10, 16(%rsp)\n"
    "mov $2, %edi\n   xor %edx, %edx\n   mov %r10, %rcx\n   mov (%rsp), %r8\n"
    "call lex_line_store\n   test %eax, %eax\n   jz .Llex_x64_fail\n"
    "add 16(%rsp), %r13\n   jmp .Llex_x64_loop\n"

    ".Llex_x64_word:\n"
    "lea lex_ordinary(%rip), %rdx\n   xor %eax, %eax\n"
    "20: movzbl (%r13,%rax), %ecx\n   cmpb $0, (%rdx,%rcx)\n   je 24f\n"
    "movzbl 1(%r13,%rax), %ecx\n   cmpb $0, (%rdx,%rcx)\n   je 23f\n"
    "movzbl 2(%r13,%rax), %ecx\n   cmpb $0, (%rdx,%rcx)\n   je 22f\n"
    "movzbl 3(%r13,%rax), %ecx\n   cmpb $0, (%rdx,%rcx)\n   je 21f\n"
    "add $4, %rax\n   jmp 20b\n"
    "21: add $3, %rax\n   jmp 24f\n"
    "22: add $2, %rax\n   jmp 24f\n"
    "23: inc %rax\n"
    "24: test %rax, %rax\n   jz .Llex_x64_slow\n"
    "movzbl (%r13,%rax), %ecx\n   test %ecx, %ecx\n   jz .Llex_x64_simple\n"
    "cmp $10, %ecx\n   je .Llex_x64_simple\n"
    "lea string_set_blanks(%rip), %rdx\n   cmpb $0, (%rdx,%rcx)\n   jne .Llex_x64_simple\n"
    "lea lex_operator(%rip), %rdx\n   cmpb $0, (%rdx,%rcx)\n   je .Llex_x64_slow\n"
    "cmp $40, %ecx\n   je .Llex_x64_slow\n"
    "cmp $60, %ecx\n   je 25f\n   cmp $62, %ecx\n   je 25f\n   jmp .Llex_x64_simple\n"
    "25: cmpb $40, 1(%r13,%rax)\n   je .Llex_x64_slow\n"

    ".Llex_x64_simple:\n"
    "mov %rax, 16(%rsp)\n   mov $1, %edi\n   xor %esi, %esi\n   mov %r13, %rdx\n"
    "mov %rax, %rcx\n   mov (%rsp), %r8\n   call lex_line_store\n"
    "test %eax, %eax\n   jz .Llex_x64_fail\n   add 16(%rsp), %r13\n"
    "jmp .Llex_x64_loop\n"

    ".Llex_x64_slow:\n"
    "mov %ebx, 16(%r14)\n   mov %r13, (%rsp)\n   lea (%rsp), %rdi\n   call lex_word\n"
    "test %eax, %eax\n   jz .Llex_x64_fail\n   mov (%rsp), %r13\n   mov (%r14), %r15\n"
    "mov 16(%r14), %ebx\n   jmp .Llex_x64_loop\n"

    ".Llex_x64_done:\n"
    "mov %r13, %r8\n   sub %r12, %r8\n   lea lex_at(%rip), %rax\n   mov %r8, (%rax)\n"
    "xor %edi, %edi\n   xor %esi, %esi\n   xor %edx, %edx\n   xor %ecx, %ecx\n"
    "call lex_line_store\n   lea -1(%rbx), %eax\n   jmp .Llex_x64_leave\n"
    ".Llex_x64_fail:\n   mov $-1, %eax\n"
    ".Llex_x64_leave:\n   add $24, %rsp\n   pop %r15\n   pop %r14\n   pop %r13\n"
    "pop %r12\n   pop %rbx\n   pop %rbp\n"
    ASM_RET
    ASM_END(lex_line_floor)
);

#elif ARM64
__asm__(
    ASM_SECTION
    ASM_LOCAL_FUNC(lex_line_store)
    ASM_ENDBR
    "ldr x8, [x24, #8]\n   add w9, w27, #2\n   cmp x9, x8\n   b.ls 1f\n"
    "stp x0, x1, [sp, #-48]!\n   stp x2, x3, [sp, #16]\n   stp x4, x30, [sp, #32]\n"
    "bl lex_grow\n   ldp x4, x30, [sp, #32]\n   ldp x2, x3, [sp, #16]\n"
    "mov w16, w0\n   ldp x0, x1, [sp]\n   add sp, sp, #48\n"
    "cbz w16, 2f\n   ldr x26, [x24]\n"
    "1:  add x8, x26, x27, lsl #5\n   str w0, [x8]\n   str w1, [x8, #4]\n"
    "str x2, [x8, #8]\n   str x3, [x8, #16]\n   str x4, [x8, #24]\n"
    "add w27, w27, #1\n   str w27, [x24, #16]\n   mov w0, #1\n"
    ASM_RET
    "2:  mov w0, #0\n"
    ASM_RET
    ASM_LOCAL_END(lex_line_store)

    ASM_FUNC(lex_line_floor)
    "stp x29, x30, [sp, #-112]!\n   mov x29, sp\n   stp x19, x20, [sp, #16]\n"
    "stp x21, x22, [sp, #32]\n   stp x23, x24, [sp, #48]\n"
    "stp x25, x26, [sp, #64]\n   stp x27, x28, [sp, #80]\n"
    "mov x19, x0\n   mov w25, w1\n"
    "adrp x24, lex_context\n   add x24, x24, :lo12:lex_context\n"
    "adrp x21, lex_ordinary\n   add x21, x21, :lo12:lex_ordinary\n"
    "adrp x22, lex_operator\n   add x22, x22, :lo12:lex_operator\n"
    "adrp x23, string_set_blanks\n   add x23, x23, :lo12:string_set_blanks\n"
    "str wzr, [x24, #16]\n   mov w27, wzr\n   ldr x26, [x24]\n   mov x20, x19\n"

    ".Llex_a64_loop:\n"
    "ldrb w8, [x20]\n   ldrb w9, [x23, x8]\n   cbz w9, 4f\n"
    "3:  add x20, x20, #1\n   ldrb w8, [x20]\n   ldrb w9, [x23, x8]\n   cbnz w9, 3b\n"
    "4:  cbz w8, .Llex_a64_done\n   cmp w8, #10\n   b.eq .Llex_a64_done\n"
    "cmp w8, #35\n   b.ne 5f\n   cbnz w25, .Llex_a64_done\n"

    "5:  sub x28, x20, x19\n   adrp x9, lex_at\n   str x28, [x9, :lo12:lex_at]\n"

    "cmp w8, #91\n   b.ne 6f\n   ldrb w9, [x20, #1]\n   cmp w9, #91\n   b.ne 6f\n"
    "ldrb w9, [x20, #2]\n   cmp w9, #32\n   b.eq 7f\n   cmp w9, #9\n   b.eq 7f\n"
    "cmp w9, #10\n   b.ne 6f\n"
    "7:  str w27, [x24, #16]\n   mov x0, x20\n   bl lex_conditional_end\n"
    "cbz x0, 6f\n   sub x3, x0, x20\n   str x3, [sp, #104]\n   mov w0, #4\n"
    "mov w1, wzr\n   mov x2, x20\n   mov x4, x28\n   bl lex_line_store\n"
    "cbz w0, .Llex_a64_fail\n   ldr x3, [sp, #104]\n   add x20, x20, x3\n"
    "b .Llex_a64_loop\n"

    "6:  ldrb w8, [x20]\n   cmp w8, #40\n   b.ne 8f\n   ldrb w9, [x20, #1]\n"
    "cmp w9, #40\n   b.ne 8f\n   str w27, [x24, #16]\n   mov x0, x20\n"
    "bl lex_arithmetic_end\n   cbz x0, 8f\n   sub x3, x0, x20\n   str x3, [sp, #104]\n"
    "mov w0, #3\n   mov w1, wzr\n   mov x2, x20\n   mov x4, x28\n"
    "bl lex_line_store\n   cbz w0, .Llex_a64_fail\n   ldr x3, [sp, #104]\n"
    "add x20, x20, x3\n   b .Llex_a64_loop\n"

    "8:  ldrb w8, [x20]\n   ldrb w9, [x22, x8]\n   cbz w9, .Llex_a64_word\n"
    "ldrb w10, [x20, #1]\n   ldrb w11, [x20, #2]\n"
    "cmp w10, #40\n   b.ne 9f\n   cmp w8, #60\n   b.eq .Llex_a64_word\n"
    "cmp w8, #62\n   b.eq .Llex_a64_word\n"

    "9:  mov w1, wzr\n   mov x3, #1\n"
    "cmp w8, #60\n   b.eq .Llex_a64_lt\n   cmp w8, #62\n   b.eq .Llex_a64_gt\n"
    "cmp w8, #38\n   b.eq .Llex_a64_amp\n   cmp w8, #124\n   b.eq .Llex_a64_pipe\n"
    "cmp w8, #59\n   b.eq .Llex_a64_semi\n   cmp w8, #40\n   b.eq .Llex_a64_lpar\n"
    "cmp w8, #41\n   b.eq .Llex_a64_rpar\n   b .Llex_a64_word\n"

    ".Llex_a64_lt:\n"
    "cmp w10, #60\n   b.ne 10f\n   cmp w11, #60\n   b.ne 11f\n"
    "mov w1, #19\n   mov x3, #3\n   b .Llex_a64_op\n"
    "11: mov w1, #4\n   mov x3, #2\n   b .Llex_a64_op\n"
    "10: cmp w10, #38\n   b.ne 12f\n   mov w1, #6\n   mov x3, #2\n   b .Llex_a64_op\n"
    "12: cmp w10, #62\n   b.ne .Llex_a64_lt1\n   mov w1, #8\n   mov x3, #2\n"
    "b .Llex_a64_op\n"
    ".Llex_a64_lt1:\n   mov w1, #13\n   b .Llex_a64_op\n"

    ".Llex_a64_gt:\n"
    "cmp w10, #62\n   b.ne 13f\n   mov w1, #5\n   mov x3, #2\n   b .Llex_a64_op\n"
    "13: cmp w10, #38\n   b.ne 14f\n   mov w1, #7\n   mov x3, #2\n   b .Llex_a64_op\n"
    "14: cmp w10, #124\n   b.ne .Llex_a64_gt1\n   mov w1, #9\n   mov x3, #2\n"
    "b .Llex_a64_op\n"
    ".Llex_a64_gt1:\n   mov w1, #14\n   b .Llex_a64_op\n"

    ".Llex_a64_amp:\n"
    "cmp w10, #62\n   b.ne 15f\n   cmp w11, #62\n   b.ne 16f\n"
    "mov w1, #18\n   mov x3, #3\n   b .Llex_a64_op\n"
    "16: mov w1, #17\n   mov x3, #2\n   b .Llex_a64_op\n"
    "15: cmp w10, #38\n   b.ne .Llex_a64_amp1\n   mov w1, #1\n   mov x3, #2\n"
    "b .Llex_a64_op\n"
    ".Llex_a64_amp1:\n   mov w1, #12\n   b .Llex_a64_op\n"

    ".Llex_a64_pipe:\n"
    "cmp w10, #124\n   b.ne 17f\n   mov w1, #2\n   mov x3, #2\n   b .Llex_a64_op\n"
    "17: cmp w10, #38\n   b.ne .Llex_a64_pipe1\n   mov w1, #20\n   mov x3, #2\n"
    "b .Llex_a64_op\n"
    ".Llex_a64_pipe1:\n   mov w1, #11\n   b .Llex_a64_op\n"

    ".Llex_a64_semi:\n"
    "cmp w10, #38\n   b.ne 18f\n   mov w1, #21\n   mov x3, #2\n   b .Llex_a64_op\n"
    "18: cmp w10, #59\n   b.ne .Llex_a64_semi1\n   cmp w11, #38\n   b.ne 19f\n"
    "mov w1, #22\n   mov x3, #3\n   b .Llex_a64_op\n"
    "19: mov w1, #3\n   mov x3, #2\n   b .Llex_a64_op\n"
    ".Llex_a64_semi1:\n   mov w1, #10\n   b .Llex_a64_op\n"

    ".Llex_a64_lpar:\n   mov w1, #15\n   b .Llex_a64_op\n"
    ".Llex_a64_rpar:\n   mov w1, #16\n"

    ".Llex_a64_op:\n"
    "cbz w1, .Llex_a64_word\n   str x3, [sp, #104]\n   mov w0, #2\n"
    "mov x2, xzr\n   mov x4, x28\n   bl lex_line_store\n   cbz w0, .Llex_a64_fail\n"
    "ldr x3, [sp, #104]\n   add x20, x20, x3\n   b .Llex_a64_loop\n"

    ".Llex_a64_word:\n"
    "ldrb w8, [x20]\n   ldrb w9, [x21, x8]\n   cbz w9, .Llex_a64_slow\n"
    "mov x16, xzr\n"
    "20: add x16, x16, #1\n   ldrb w8, [x20, x16]\n   ldrb w9, [x21, x8]\n"
    "cbnz w9, 20b\n"
    "cbz w8, .Llex_a64_simple\n   cmp w8, #10\n   b.eq .Llex_a64_simple\n"
    "ldrb w9, [x23, x8]\n   cbnz w9, .Llex_a64_simple\n"
    "ldrb w9, [x22, x8]\n   cbz w9, .Llex_a64_slow\n"
    "cmp w8, #40\n   b.eq .Llex_a64_slow\n"
    "cmp w8, #60\n   b.eq 25f\n   cmp w8, #62\n   b.eq 25f\n   b .Llex_a64_simple\n"
    "25: add x9, x20, x16\n   ldrb w9, [x9, #1]\n   cmp w9, #40\n   b.eq .Llex_a64_slow\n"

    ".Llex_a64_simple:\n"
    "str x16, [sp, #104]\n   mov w0, #1\n   mov w1, wzr\n   mov x2, x20\n"
    "mov x3, x16\n   mov x4, x28\n   bl lex_line_store\n   cbz w0, .Llex_a64_fail\n"
    "ldr x16, [sp, #104]\n   add x20, x20, x16\n   b .Llex_a64_loop\n"

    ".Llex_a64_slow:\n"
    "str w27, [x24, #16]\n   str x20, [sp, #96]\n   add x0, sp, #96\n   bl lex_word\n"
    "cbz w0, .Llex_a64_fail\n   ldr x20, [sp, #96]\n   ldr x26, [x24]\n"
    "ldr w27, [x24, #16]\n   b .Llex_a64_loop\n"

    ".Llex_a64_done:\n"
    "sub x28, x20, x19\n   adrp x9, lex_at\n   str x28, [x9, :lo12:lex_at]\n"
    "mov w0, wzr\n   mov w1, wzr\n   mov x2, xzr\n   mov x3, xzr\n   mov x4, x28\n"
    "bl lex_line_store\n   sub w0, w27, #1\n   b .Llex_a64_leave\n"
    ".Llex_a64_fail:\n   mov w0, #-1\n"
    ".Llex_a64_leave:\n   ldp x27, x28, [sp, #80]\n   ldp x25, x26, [sp, #64]\n"
    "ldp x23, x24, [sp, #48]\n   ldp x21, x22, [sp, #32]\n"
    "ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #112\n"
    ASM_RET
    ASM_END(lex_line_floor)
);

#elif RISCV64
__asm__(
    ASM_SECTION
    ASM_LOCAL_FUNC(lex_line_store)
    "ld t0, 8(s2)\n   addi t1, s6, 2\n   bgeu t0, t1, 1f\n"
    "addi sp, sp, -48\n   sd a0, 0(sp)\n   sd a1, 8(sp)\n   sd a2, 16(sp)\n"
    "sd a3, 24(sp)\n   sd a4, 32(sp)\n   sd ra, 40(sp)\n   call lex_grow\n"
    "ld ra, 40(sp)\n   mv t3, a0\n   ld a0, 0(sp)\n   ld a1, 8(sp)\n"
    "ld a2, 16(sp)\n   ld a3, 24(sp)\n   ld a4, 32(sp)\n   addi sp, sp, 48\n"
    "beqz t3, 2f\n   ld s8, 0(s2)\n"
    "1:  slli t0, s6, 5\n   add t0, s8, t0\n   sw a0, 0(t0)\n   sw a1, 4(t0)\n"
    "sd a2, 8(t0)\n   sd a3, 16(t0)\n   sd a4, 24(t0)\n   addi s6, s6, 1\n"
    "sw s6, 16(s2)\n   li a0, 1\n"
    ASM_RET
    "2:  li a0, 0\n"
    ASM_RET
    ASM_LOCAL_END(lex_line_store)

    ASM_FUNC(lex_line_floor)
    "addi sp, sp, -96\n   sd ra, 0(sp)\n   sd s0, 8(sp)\n   sd s1, 16(sp)\n"
    "sd s2, 24(sp)\n   sd s3, 32(sp)\n   sd s4, 40(sp)\n   sd s5, 48(sp)\n"
    "sd s6, 56(sp)\n   sd s7, 64(sp)\n   sd s8, 72(sp)\n"
    "mv s0, a0\n   mv s7, a1\n   lla s2, lex_context\n   lla s3, lex_ordinary\n"
    "lla s4, lex_operator\n   lla s5, string_set_blanks\n   sw zero, 16(s2)\n"
    "li s6, 0\n   ld s8, 0(s2)\n   mv s1, s0\n"

    ".Llex_rv_loop:\n"
    "lbu t0, 0(s1)\n   add t1, s5, t0\n   lbu t1, 0(t1)\n   beqz t1, 4f\n"
    "3:  addi s1, s1, 1\n   lbu t0, 0(s1)\n   add t1, s5, t0\n   lbu t1, 0(t1)\n"
    "bnez t1, 3b\n"
    "4:  beqz t0, .Llex_rv_done\n   li t1, 10\n   beq t0, t1, .Llex_rv_done\n"
    "li t1, 35\n   bne t0, t1, 5f\n   bnez s7, .Llex_rv_done\n"

    "5:  sub t3, s1, s0\n   lla t1, lex_at\n   sd t3, 0(t1)\n   sd t3, 80(sp)\n"

    "li t1, 91\n   bne t0, t1, 6f\n   lbu t1, 1(s1)\n   li t2, 91\n   bne t1, t2, 6f\n"
    "lbu t1, 2(s1)\n   li t2, 32\n   beq t1, t2, 7f\n   li t2, 9\n   beq t1, t2, 7f\n"
    "li t2, 10\n   bne t1, t2, 6f\n"
    "7:  sw s6, 16(s2)\n   mv a0, s1\n   call lex_conditional_end\n"
    "beqz a0, 6f\n   sub a3, a0, s1\n   li a0, 4\n   li a1, 0\n   mv a2, s1\n"
    "ld a4, 80(sp)\n   sd a3, 88(sp)\n   call lex_line_store\n   beqz a0, .Llex_rv_fail\n"
    "ld a3, 88(sp)\n   add s1, s1, a3\n   j .Llex_rv_loop\n"

    "6:  lbu t0, 0(s1)\n   li t1, 40\n   bne t0, t1, 8f\n   lbu t1, 1(s1)\n"
    "li t2, 40\n   bne t1, t2, 8f\n   sw s6, 16(s2)\n   mv a0, s1\n"
    "call lex_arithmetic_end\n   beqz a0, 8f\n   sub a3, a0, s1\n   li a0, 3\n"
    "li a1, 0\n   mv a2, s1\n   ld a4, 80(sp)\n   sd a3, 88(sp)\n"
    "call lex_line_store\n   beqz a0, .Llex_rv_fail\n   ld a3, 88(sp)\n"
    "add s1, s1, a3\n   j .Llex_rv_loop\n"

    "8:  lbu t0, 0(s1)\n   add t1, s4, t0\n   lbu t1, 0(t1)\n   beqz t1, .Llex_rv_word\n"
    "lbu t1, 1(s1)\n   lbu t2, 2(s1)\n   li t4, 40\n   bne t1, t4, 9f\n"
    "li t4, 60\n   beq t0, t4, .Llex_rv_word\n   li t4, 62\n   beq t0, t4, .Llex_rv_word\n"

    "9:  li a1, 0\n   li a3, 1\n   li t4, 60\n   beq t0, t4, .Llex_rv_lt\n"
    "li t4, 62\n   beq t0, t4, .Llex_rv_gt\n   li t4, 38\n   beq t0, t4, .Llex_rv_amp\n"
    "li t4, 124\n   beq t0, t4, .Llex_rv_pipe\n   li t4, 59\n   beq t0, t4, .Llex_rv_semi\n"
    "li t4, 40\n   beq t0, t4, .Llex_rv_lpar\n   li t4, 41\n   beq t0, t4, .Llex_rv_rpar\n"
    "j .Llex_rv_word\n"

    ".Llex_rv_lt:\n"
    "li t4, 60\n   bne t1, t4, 10f\n   bne t2, t4, 11f\n"
    "li a1, 19\n   li a3, 3\n   j .Llex_rv_op\n"
    "11: li a1, 4\n   li a3, 2\n   j .Llex_rv_op\n"
    "10: li t4, 38\n   bne t1, t4, 12f\n   li a1, 6\n   li a3, 2\n   j .Llex_rv_op\n"
    "12: li t4, 62\n   bne t1, t4, .Llex_rv_lt1\n   li a1, 8\n   li a3, 2\n"
    "j .Llex_rv_op\n"
    ".Llex_rv_lt1:\n   li a1, 13\n   j .Llex_rv_op\n"

    ".Llex_rv_gt:\n"
    "li t4, 62\n   bne t1, t4, 13f\n   li a1, 5\n   li a3, 2\n   j .Llex_rv_op\n"
    "13: li t4, 38\n   bne t1, t4, 14f\n   li a1, 7\n   li a3, 2\n   j .Llex_rv_op\n"
    "14: li t4, 124\n   bne t1, t4, .Llex_rv_gt1\n   li a1, 9\n   li a3, 2\n"
    "j .Llex_rv_op\n"
    ".Llex_rv_gt1:\n   li a1, 14\n   j .Llex_rv_op\n"

    ".Llex_rv_amp:\n"
    "li t4, 62\n   bne t1, t4, 15f\n   bne t2, t4, 16f\n"
    "li a1, 18\n   li a3, 3\n   j .Llex_rv_op\n"
    "16: li a1, 17\n   li a3, 2\n   j .Llex_rv_op\n"
    "15: li t4, 38\n   bne t1, t4, .Llex_rv_amp1\n   li a1, 1\n   li a3, 2\n"
    "j .Llex_rv_op\n"
    ".Llex_rv_amp1:\n   li a1, 12\n   j .Llex_rv_op\n"

    ".Llex_rv_pipe:\n"
    "li t4, 124\n   bne t1, t4, 17f\n   li a1, 2\n   li a3, 2\n   j .Llex_rv_op\n"
    "17: li t4, 38\n   bne t1, t4, .Llex_rv_pipe1\n   li a1, 20\n   li a3, 2\n"
    "j .Llex_rv_op\n"
    ".Llex_rv_pipe1:\n   li a1, 11\n   j .Llex_rv_op\n"

    ".Llex_rv_semi:\n"
    "li t4, 38\n   bne t1, t4, 18f\n   li a1, 21\n   li a3, 2\n   j .Llex_rv_op\n"
    "18: li t4, 59\n   bne t1, t4, .Llex_rv_semi1\n   li t4, 38\n   bne t2, t4, 19f\n"
    "li a1, 22\n   li a3, 3\n   j .Llex_rv_op\n"
    "19: li a1, 3\n   li a3, 2\n   j .Llex_rv_op\n"
    ".Llex_rv_semi1:\n   li a1, 10\n   j .Llex_rv_op\n"

    ".Llex_rv_lpar:\n   li a1, 15\n   j .Llex_rv_op\n"
    ".Llex_rv_rpar:\n   li a1, 16\n"

    ".Llex_rv_op:\n"
    "beqz a1, .Llex_rv_word\n   li a0, 2\n   li a2, 0\n   ld a4, 80(sp)\n"
    "sd a3, 88(sp)\n   call lex_line_store\n   beqz a0, .Llex_rv_fail\n"
    "ld a3, 88(sp)\n   add s1, s1, a3\n   j .Llex_rv_loop\n"

    ".Llex_rv_word:\n"
    "lbu t0, 0(s1)\n   add t1, s3, t0\n   lbu t1, 0(t1)\n   beqz t1, .Llex_rv_slow\n"
    "li t5, 0\n"
    "20: addi t5, t5, 1\n   add t0, s1, t5\n   lbu t0, 0(t0)\n   add t1, s3, t0\n"
    "lbu t1, 0(t1)\n   bnez t1, 20b\n"
    "beqz t0, .Llex_rv_simple\n   li t1, 10\n   beq t0, t1, .Llex_rv_simple\n"
    "add t1, s5, t0\n   lbu t1, 0(t1)\n   bnez t1, .Llex_rv_simple\n"
    "add t1, s4, t0\n   lbu t1, 0(t1)\n   beqz t1, .Llex_rv_slow\n"
    "li t1, 40\n   beq t0, t1, .Llex_rv_slow\n"
    "li t1, 60\n   beq t0, t1, 25f\n   li t1, 62\n   beq t0, t1, 25f\n"
    "j .Llex_rv_simple\n"
    "25: add t1, s1, t5\n   lbu t1, 1(t1)\n   li t2, 40\n   beq t1, t2, .Llex_rv_slow\n"

    ".Llex_rv_simple:\n"
    "li a0, 1\n   li a1, 0\n   mv a2, s1\n   mv a3, t5\n   ld a4, 80(sp)\n"
    "sd t5, 88(sp)\n   call lex_line_store\n   beqz a0, .Llex_rv_fail\n"
    "ld a3, 88(sp)\n   add s1, s1, a3\n   j .Llex_rv_loop\n"

    ".Llex_rv_slow:\n"
    "sw s6, 16(s2)\n   sd s1, 80(sp)\n   addi a0, sp, 80\n   call lex_word\n"
    "beqz a0, .Llex_rv_fail\n   ld s1, 80(sp)\n   ld s8, 0(s2)\n"
    "lwu s6, 16(s2)\n   j .Llex_rv_loop\n"

    ".Llex_rv_done:\n"
    "sub a4, s1, s0\n   lla t1, lex_at\n   sd a4, 0(t1)\n   li a0, 0\n   li a1, 0\n"
    "li a2, 0\n   li a3, 0\n   call lex_line_store\n   addi a0, s6, -1\n"
    "j .Llex_rv_leave\n"
    ".Llex_rv_fail:\n   li a0, -1\n"
    ".Llex_rv_leave:\n   ld ra, 0(sp)\n   ld s0, 8(sp)\n   ld s1, 16(sp)\n"
    "ld s2, 24(sp)\n   ld s3, 32(sp)\n   ld s4, 40(sp)\n   ld s5, 48(sp)\n"
    "ld s6, 56(sp)\n   ld s7, 64(sp)\n   ld s8, 72(sp)\n   addi sp, sp, 96\n"
    ASM_RET
    ASM_END(lex_line_floor)
);
#endif
