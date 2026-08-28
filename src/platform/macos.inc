/*
        Experimental C standard library

        macOS platform specific functions and definitions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_MACOS
#define STANDARD_MODERN_C_PLATFORM_MACOS

/*
        XNU has no nanosleep syscall: libsystem builds it out of
        __semwait_signal. select with no descriptors and a timeout is a real
        syscall that sleeps for the requested interval, which is all this
        needs to do.

        This is deliberately Mach-O assembly rather than the ELF-shaped
        ASM_FUNC/ASM_END vocabulary in library.c. Darwin C symbols have a
        leading underscore, Mach-O has no ELF .type/.size directives, and the
        linker entry named _start must really be _start rather than the
        __start a C function named _start would emit.

        The BSD syscall class is 0x02000000. select is call 93 and exit is call
        1 on both Darwin architectures. x86_64 carries that number in rax and
        uses syscall; arm64 carries it in x16 and uses svc #0x80.

        Dividing nanoseconds by 1000 uses the same reciprocal reduction clang
        selects for the full unsigned 64-bit range: discard the three bits
        made irrelevant by 1000 == 8 * 125, multiply-high by the reciprocal of
        125, then shift four. It avoids a hardware divide on both floors.

        LC_MAIN is entered by dyld with argc, argv, envp and apple in the first
        four C argument registers. The retained kernel vector still has argc
        in the word immediately before argv, so _start stores argv - 8 as
        program_stack_base. That preserves the stack-vector contract used by
        program_argument and program_environment while keeping the live call
        stack independent and correctly aligned. This was checked at runtime
        on native arm64 and Rosetta x86_64; both had argv[-1] == argc and envp
        == argv + argc + 1.

        Startup then detects the x86 vector floor before any dispatched
        routine can run. __start is a zero-cost second label preserving the
        symbol emitted by the former C spelling; the actual linker entry is
        _start.

        A successful exit syscall cannot return. If it does, _start lands on
        an explicit trap instead of treating argc as an x86 return address or
        re-executing the arm64 branch forever.
*/
#if defined(X64)
#if defined(__CET__) && (__CET__ & 1)
#define MACOS_X64_ENDBR "endbr64\n"
#else
#define MACOS_X64_ENDBR ""
#endif
__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".p2align 4\n"
    ".globl _sleep\n"
    "_sleep:\n"
    MACOS_X64_ENDBR
    "        subq    $16, %rsp\n"
    "        movq    (%rdi), %rax\n"
    "        movq    %rax, (%rsp)\n"
    "        movq    8(%rdi), %rax\n"
    "        shrq    $3, %rax\n"
    "        movabsq $0x20c49ba5e353f7cf, %rcx\n"
    "        mulq    %rcx\n"
    "        shrq    $4, %rdx\n"
    "        movl    %edx, 8(%rsp)\n"
    "        movq    %rsp, %r8\n"
    "        xorl    %edi, %edi\n"
    "        xorl    %esi, %esi\n"
    "        xorl    %edx, %edx\n"
    "        xorl    %r10d, %r10d\n"
    "        movl    $0x0200005d, %eax\n"
    "        syscall\n"
    "        addq    $16, %rsp\n"
    "        retq\n"

    ".p2align 4\n"
    ".globl _exit\n"
    "_exit:\n"
    MACOS_X64_ENDBR
    "        movslq  %edi, %rdi\n"
    "        movl    $0x02000001, %eax\n"
    "        syscall\n"
    "        retq\n"

    ".p2align 4\n"
    ".globl _start\n"
    ".globl __start\n"
    "_start:\n"
    "__start:\n"
    MACOS_X64_ENDBR
    "        xorl    %ebp, %ebp\n"
    "        leaq    -8(%rsi), %rax\n"
    "        movq    %rax, _program_stack_base(%rip)\n"
    "        andq    $-16, %rsp\n"
    "        callq   _moonwater_cpu_detect\n"
    "        callq   _main\n"
    "        movl    %eax, %edi\n"
    "        callq   _exit\n"
    "        ud2\n"
);
#undef MACOS_X64_ENDBR
#elif defined(ARM64)
#if defined(__ARM_FEATURE_BTI_DEFAULT)
#define MACOS_ARM64_BTI "bti c\n"
#else
#define MACOS_ARM64_BTI ""
#endif
__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".p2align 2\n"
    ".globl _sleep\n"
    "_sleep:\n"
    MACOS_ARM64_BTI
    "        sub     sp, sp, #16\n"
    "        ldp     x8, x9, [x0]\n"
    "        str     x8, [sp]\n"
    "        lsr     x8, x9, #3\n"
    "        mov     x9, #0xf7cf\n"
    "        movk    x9, #0xe353, lsl #16\n"
    "        movk    x9, #0x9ba5, lsl #32\n"
    "        movk    x9, #0x20c4, lsl #48\n"
    "        umulh   x8, x8, x9\n"
    "        lsr     x8, x8, #4\n"
    "        str     w8, [sp, #8]\n"
    "        mov     x4, sp\n"
    "        mov     x0, xzr\n"
    "        mov     x1, xzr\n"
    "        mov     x2, xzr\n"
    "        mov     x3, xzr\n"
    "        mov     w16, #0x5d\n"
    "        movk    w16, #0x200, lsl #16\n"
    "        svc     #0x80\n"
    "        add     sp, sp, #16\n"
    "        ret\n"

    ".p2align 2\n"
    ".globl _exit\n"
    "_exit:\n"
    MACOS_ARM64_BTI
    "        sxtw    x0, w0\n"
    "        mov     w16, #1\n"
    "        movk    w16, #0x200, lsl #16\n"
    "        svc     #0x80\n"
    "        ret\n"

    ".p2align 2\n"
    ".globl _start\n"
    ".globl __start\n"
    "_start:\n"
    "__start:\n"
    MACOS_ARM64_BTI
    "        mov     x29, xzr\n"
    "        mov     x30, xzr\n"
    "        sub     x9, x1, #8\n"
    "        adrp    x8, _program_stack_base@PAGE\n"
    "        str     x9, [x8, _program_stack_base@PAGEOFF]\n"
    "        bl      _moonwater_cpu_detect\n"
    "        bl      _main\n"
    "        bl      _exit\n"
    "        brk     #0\n"
);
#undef MACOS_ARM64_BTI
#else
#error "macOS runtime assembly needs x86_64 or arm64"
#endif

#endif
