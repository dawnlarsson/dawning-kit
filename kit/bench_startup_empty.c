/*
        The host-kernel floor for startup measurements: enter, issue exit(0),
        and do nothing else.  This deliberately does not include Moonwater's
        runtime, so its process/image cost can be subtracted from the tiny
        main in bench_tiny.c rather than guessed from a full shell.
*/
__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start, @function\n"
    "_start:\n"
#if defined(__x86_64__)
    "xor %edi, %edi\n"
    "mov $60, %eax\n"
    "syscall\n"
#elif defined(__aarch64__)
    "mov x0, #0\n"
    "mov x8, #93\n"
    "svc #0\n"
#elif defined(__riscv)
    "li a0, 0\n"
    "li a7, 93\n"
    "ecall\n"
#else
#error unsupported startup benchmark architecture
#endif
    ".size _start, .-_start\n"
);
