/*
        Experimental C standard library

        Linux platform specific functions and definitions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_LINUX
#define STANDARD_MODERN_C_PLATFORM_LINUX

struct linux_dirent64
{
        p64 d_ino;
        p64 d_off;
        p16 d_reclen;
        p8 d_type;
        p8 d_name[];
};

positive2 term_size();
positive system_write_all(positive handle, address_any data, positive length);
bipolar system_read_retry(positive handle, address_any data, positive length);
bipolar system_wait4_retry(bipolar process, address_any status,
                           positive options, address_any usage);
bipolar file_slurp(string_address path, p8 address_to into, positive capacity);

#define LINUX_RUNTIME_TEXT_INNER(value) #value
#define LINUX_RUNTIME_TEXT(value) LINUX_RUNTIME_TEXT_INNER(value)

/*
        Linux enters _start with the original process stack, not through the C
        ABI. _start records that exact address before aligning the call stack,
        detects the CPU before any dispatched primitive runs, and passes
        main's signed 32-bit result to exit. There is no C-side startup shim
        and no extra call between the kernel entry and that sequence.

        term_size returns the two-word positive2 in the architecture's normal
        aggregate return registers. Its eight-byte winsize scratch stays on
        the aligned stack, and only a zero ioctl result replaces the 80 by 24
        fallback. system_write_all keeps issuing write(2) from the first byte
        not yet accepted while the syscall makes positive progress, returns
        the number accepted, and deliberately does not retry zero, EINTR, or
        any other negative result. A zero length makes no syscall. That is the
        common contract of the shell's former write loops, not a stronger I/O
        policy hidden behind their name.

        system_read_retry is the complementary single-read primitive: it
        retries exactly EINTR, including for a zero length, and returns every
        other kernel result unchanged. Its implementation deliberately leaves
        the data and length argument registers, and the spare caller-saved
        registers used below, untouched. file_slurp keeps its loop state there
        across the direct call: arm64 and riscv64 stay stackless, while x86_64
        reserves just the one word its call-site alignment requires.

        system_wait4_retry likewise retries only EINTR and otherwise exposes
        wait4's result unchanged. x86_64 moves the ABI's fourth argument from
        rcx to the syscall ABI's r10 before trapping. arm64 and riscv64 keep
        status, options and rusage in their ABI registers and reload the pid
        that each trap replaces with its result.

        file_slurp reserves one byte for a terminator and fills the rest through
        EOF or capacity. Zero capacity is a query that touches neither pointer
        and makes no syscall; capacity one still validates the path, writes the
        terminator and closes it. openat and read retry EINTR. An open error
        leaves the destination untouched; another read error terminates what
        was accepted, closes once and returns that error. close itself is never
        retried: after EINTR Linux no longer gives userspace a portable answer
        about ownership of the descriptor.

        sleep and exit trap directly rather than paying for a generic
        syscall-arity adapter.
*/
#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "xor %r8d, %r8d\n   test %rdx, %rdx\n"
    "jz .Lsystem_write_all_x64_done\n"
    ".Lsystem_write_all_x64_more:\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(write)) ", %eax\n   syscall\n"
    "test %rax, %rax\n   jle .Lsystem_write_all_x64_done\n"
    "add %rax, %r8\n   add %rax, %rsi\n   sub %rax, %rdx\n"
    "jnz .Lsystem_write_all_x64_more\n"
    ".Lsystem_write_all_x64_done:\n"
    "mov %r8, %rax\n"
    ASM_RET
    ASM_END(system_write_all)

    ASM_FUNC(system_read_retry)
    ".Lsystem_read_retry_x64_more:\n"
    "xor %eax, %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lsystem_read_retry_x64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mov %rcx, %r10\n"
    ".Lsystem_wait4_retry_x64_more:\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(wait4)) ", %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lsystem_wait4_retry_x64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "test %rdx, %rdx\n   jz .Lfile_slurp_x64_zero\n"
    "mov %rsi, %r8\n   mov %rdx, %r9\n   mov %rdi, %rsi\n"
    ".Lfile_slurp_x64_open:\n"
    "mov $-100, %rdi\n   xor %edx, %edx\n   xor %r10d, %r10d\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(openat)) ", %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lfile_slurp_x64_open\n"
    "test %rax, %rax\n   js .Lfile_slurp_x64_return\n"
    "mov %rax, %rdi\n   mov %r8, %rsi\n   dec %r9\n"
    "jnz .Lfile_slurp_x64_save\n"
    "movb $0, (%rsi)\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(close)) ", %eax\n   syscall\n"
    "xor %eax, %eax\n"
    ASM_RET
    ".Lfile_slurp_x64_save:\n"
    "mov %r9, %rdx\n   xor %r8d, %r8d\n   sub $8, %rsp\n"
    ".Lfile_slurp_x64_read:\n"
    "call system_read_retry\n"
    "test %rax, %rax\n   jz .Lfile_slurp_x64_terminate\n"
    "js .Lfile_slurp_x64_read_error\n"
    "add %rax, %rsi\n   add %rax, %r8\n   sub %rax, %rdx\n"
    "jnz .Lfile_slurp_x64_read\n"
    "jmp .Lfile_slurp_x64_terminate\n"
    ".Lfile_slurp_x64_read_error:\n"
    "mov %rax, %r8\n"
    ".Lfile_slurp_x64_terminate:\n"
    "movb $0, (%rsi)\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(close)) ", %eax\n   syscall\n"
    "mov %r8, %rax\n   add $8, %rsp\n"
    ASM_RET
    ".Lfile_slurp_x64_zero:\n"
    "xor %eax, %eax\n"
    ".Lfile_slurp_x64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "sub $16, %rsp\n   mov $1, %edi\n   mov $0x5413, %esi\n"
    "mov %rsp, %rdx\n   mov $" LINUX_RUNTIME_TEXT(syscall(ioctl)) ", %eax\n"
    "syscall\n   test %rax, %rax\n   jnz .Lterm_size_x64_default\n"
    "movzwq 2(%rsp), %rax\n   movzwq (%rsp), %rdx\n"
    "jmp .Lterm_size_x64_done\n"
    ".Lterm_size_x64_default:\n"
    "mov $80, %eax\n   mov $24, %edx\n"
    ".Lterm_size_x64_done:\n"
    "add $16, %rsp\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "xor %esi, %esi\n   xor %edx, %edx\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(nanosleep)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "movslq %edi, %rdi\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(exit)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(_start)
    "xor %ebp, %ebp\n   mov %rsp, program_stack_base(%rip)\n"
    "and $-16, %rsp\n   call moonwater_cpu_detect\n   call main\n"
    "mov %eax, %edi\n   call exit\n   hlt\n"
    ASM_END(_start)
);
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "mov x3, x0\n   mov x4, xzr\n   cbz x2, .Lsystem_write_all_arm64_done\n"
    ".Lsystem_write_all_arm64_more:\n"
    "mov x0, x3\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(write)) "\n"
    SYSCALL_INSTRUCTION "\n   cmp x0, #0\n"
    "b.le .Lsystem_write_all_arm64_done\n"
    "add x4, x4, x0\n   add x1, x1, x0\n   sub x2, x2, x0\n"
    "cbnz x2, .Lsystem_write_all_arm64_more\n"
    ".Lsystem_write_all_arm64_done:\n"
    "mov x0, x4\n"
    ASM_RET
    ASM_END(system_write_all)

    ASM_FUNC(system_read_retry)
    "mov x3, x0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(read)) "\n"
    ".Lsystem_read_retry_arm64_more:\n"
    "mov x0, x3\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n"
    "b.eq .Lsystem_read_retry_arm64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mov x4, x0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(wait4)) "\n"
    ".Lsystem_wait4_retry_arm64_more:\n"
    "mov x0, x4\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n"
    "b.eq .Lsystem_wait4_retry_arm64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "cbz x2, .Lfile_slurp_arm64_zero\n"
    "mov x4, x1\n   mov x5, x2\n   mov x6, x0\n"
    ".Lfile_slurp_arm64_open:\n"
    "mov x0, #-100\n   mov x1, x6\n   mov x2, xzr\n   mov x3, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(openat)) "\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n   b.eq .Lfile_slurp_arm64_open\n"
    "tbnz x0, #63, .Lfile_slurp_arm64_return\n"
    "mov x3, x0\n   sub x5, x5, #1\n"
    "cbnz x5, .Lfile_slurp_arm64_save\n"
    "strb wzr, [x4]\n   mov x0, x3\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(close)) "\n"
    SYSCALL_INSTRUCTION "\n   mov x0, xzr\n"
    ASM_RET
    ".Lfile_slurp_arm64_save:\n"
    "mov x1, x4\n   mov x2, x5\n   mov x4, x3\n   mov x5, xzr\n"
    "mov x6, x30\n"
    ".Lfile_slurp_arm64_read:\n"
    "mov x0, x4\n"
    "bl system_read_retry\n"
    "cbz x0, .Lfile_slurp_arm64_terminate\n"
    "tbnz x0, #63, .Lfile_slurp_arm64_read_error\n"
    "add x1, x1, x0\n   add x5, x5, x0\n   sub x2, x2, x0\n"
    "cbnz x2, .Lfile_slurp_arm64_read\n"
    "b .Lfile_slurp_arm64_terminate\n"
    ".Lfile_slurp_arm64_read_error:\n"
    "mov x5, x0\n"
    ".Lfile_slurp_arm64_terminate:\n"
    "strb wzr, [x1]\n   mov x0, x4\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(close)) "\n"
    SYSCALL_INSTRUCTION "\n   mov x0, x5\n   ret x6\n"
    ".Lfile_slurp_arm64_zero:\n"
    "mov x0, xzr\n"
    ".Lfile_slurp_arm64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "sub sp, sp, #16\n   mov x0, #1\n   mov x1, #0x5413\n"
    "mov x2, sp\n   mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(ioctl)) "\n"
    SYSCALL_INSTRUCTION "\n   cbnz x0, .Lterm_size_arm64_default\n"
    "ldrh w0, [sp, #2]\n   ldrh w1, [sp]\n"
    "b .Lterm_size_arm64_done\n"
    ".Lterm_size_arm64_default:\n"
    "mov x0, #80\n   mov x1, #24\n"
    ".Lterm_size_arm64_done:\n"
    "add sp, sp, #16\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "mov x1, xzr\n   mov x2, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(nanosleep)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "sxtw x0, w0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(exit)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(_start)
    "mov x29, #0\n   mov x30, #0\n   mov x0, sp\n"
    "adrp x1, program_stack_base\n   str x0, [x1, :lo12:program_stack_base]\n"
    "and sp, x0, #-16\n   bl moonwater_cpu_detect\n   bl main\n"
    "bl exit\n   brk #0\n"
    ASM_END(_start)
);
#elif RISCV64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "mv t0, a0\n   li t1, 0\n   beqz a2, .Lsystem_write_all_riscv64_done\n"
    ".Lsystem_write_all_riscv64_more:\n"
    "mv a0, t0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(write)) "\n   ecall\n"
    "blez a0, .Lsystem_write_all_riscv64_done\n"
    "add t1, t1, a0\n   add a1, a1, a0\n   sub a2, a2, a0\n"
    "bnez a2, .Lsystem_write_all_riscv64_more\n"
    ".Lsystem_write_all_riscv64_done:\n"
    "mv a0, t1\n"
    ASM_RET
    ASM_END(system_write_all)

    ASM_FUNC(system_read_retry)
    "mv t1, a0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(read)) "\n"
    "li t0, -4\n"
    ".Lsystem_read_retry_riscv64_more:\n"
    "mv a0, t1\n   ecall\n"
    "beq a0, t0, .Lsystem_read_retry_riscv64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mv t0, a0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(wait4)) "\n"
    "li t1, -4\n"
    ".Lsystem_wait4_retry_riscv64_more:\n"
    "mv a0, t0\n   ecall\n"
    "beq a0, t1, .Lsystem_wait4_retry_riscv64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "beqz a2, .Lfile_slurp_riscv64_zero\n"
    "mv t0, a0\n   mv t1, a1\n   mv t2, a2\n"
    ".Lfile_slurp_riscv64_open:\n"
    "li a0, -100\n   mv a1, t0\n   li a2, 0\n   li a3, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(openat)) "\n   ecall\n"
    "li t3, -4\n   beq a0, t3, .Lfile_slurp_riscv64_open\n"
    "bltz a0, .Lfile_slurp_riscv64_return\n"
    "mv t3, a0\n   addi t2, t2, -1\n"
    "bnez t2, .Lfile_slurp_riscv64_save\n"
    "sb zero, 0(t1)\n   mv a0, t3\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(close)) "\n   ecall\n"
    "li a0, 0\n"
    ASM_RET
    ".Lfile_slurp_riscv64_save:\n"
    "mv a1, t1\n   mv a2, t2\n   mv t2, t3\n   li t3, 0\n"
    "mv t4, ra\n"
    ".Lfile_slurp_riscv64_read:\n"
    "mv a0, t2\n"
    "call system_read_retry\n"
    "beqz a0, .Lfile_slurp_riscv64_terminate\n"
    "bltz a0, .Lfile_slurp_riscv64_read_error\n"
    "add a1, a1, a0\n   add t3, t3, a0\n   sub a2, a2, a0\n"
    "bnez a2, .Lfile_slurp_riscv64_read\n"
    "j .Lfile_slurp_riscv64_terminate\n"
    ".Lfile_slurp_riscv64_read_error:\n"
    "mv t3, a0\n"
    ".Lfile_slurp_riscv64_terminate:\n"
    "sb zero, 0(a1)\n   mv a0, t2\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(close)) "\n   ecall\n"
    "mv a0, t3\n   mv ra, t4\n"
    ASM_RET
    ".Lfile_slurp_riscv64_zero:\n"
    "li a0, 0\n"
    ".Lfile_slurp_riscv64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "addi sp, sp, -16\n   li a0, 1\n   li a1, 0x5413\n"
    "mv a2, sp\n   li a7, " LINUX_RUNTIME_TEXT(syscall(ioctl)) "\n   ecall\n"
    "bnez a0, .Lterm_size_riscv64_default\n"
    "lhu a0, 2(sp)\n   lhu a1, 0(sp)\n"
    "j .Lterm_size_riscv64_done\n"
    ".Lterm_size_riscv64_default:\n"
    "li a0, 80\n   li a1, 24\n"
    ".Lterm_size_riscv64_done:\n"
    "addi sp, sp, 16\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "li a1, 0\n   li a2, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(nanosleep)) "\n   ecall\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "sext.w a0, a0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(exit)) "\n   ecall\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(_start)
    /* The RISC-V psABI reserves gp for __global_pointer$.  GNU ld relaxes
       accesses to nearby small globals into gp-relative loads and stores,
       including C generated after this entry point.  A kernel enters with
       gp unspecified, so establish it before the first call.  norelax is
       essential here: relaxing the initializer itself into mv gp,gp would
       preserve the unknown incoming value. */
    ".option push\n   .option norelax\n"
    "lla gp, __global_pointer$\n"
    ".option pop\n"
    "li s0, 0\n   li ra, 0\n   mv a0, sp\n"
    "lla t0, program_stack_base\n   sd a0, 0(t0)\n"
    "andi sp, sp, -16\n   call moonwater_cpu_detect\n   call main\n"
    "call exit\n   ebreak\n"
    ASM_END(_start)
);
#else
#error "no _start defined for this architecture"
#endif

#undef LINUX_RUNTIME_TEXT
#undef LINUX_RUNTIME_TEXT_INNER

#endif
