/*
        Native lower bounds for kit/profile_shell.

        The ordinary build exits immediately: it is the least user-space work
        a static process can retire after exec.  FLOOR_CONTROL adds the exact
        20,000 backedges used by the shell loop workload, but none of the
        shell semantics.  That makes it a strict control-flow lower bound,
        not a competing shell and not a claim that parsing and variables are
        free.
*/

        .text
        .globl _start
        .type _start, @function
_start:
#if defined(FLOOR_CONTROL)
# if defined(__x86_64__)
        mov $20000, %ecx
1:      dec %ecx
        jnz 1b
# elif defined(__aarch64__)
        mov x1, #20000
1:      subs x1, x1, #1
        b.ne 1b
# elif defined(__riscv) && __riscv_xlen == 64
        li t0, 20000
1:      addi t0, t0, -1
        bnez t0, 1b
# else
#  error unsupported architecture
# endif
#endif

#if defined(__x86_64__)
        mov $60, %eax
        xor %edi, %edi
        syscall
#elif defined(__aarch64__)
        mov x8, #93
        mov x0, #0
        svc #0
#elif defined(__riscv) && __riscv_xlen == 64
        li a7, 93
        li a0, 0
        ecall
#else
# error unsupported architecture
#endif

        .size _start, .-_start
