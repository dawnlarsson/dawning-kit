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
        policy hidden behind their name. sleep and exit trap directly rather
        than paying for a generic syscall-arity adapter.
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
