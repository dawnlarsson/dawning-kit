/*
        Dawning Experimental C standard library

        Linux platform specific functions and definitions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef DAWN_MODERN_C_PLATFORM_LINUX
#define DAWN_MODERN_C_PLATFORM_LINUX

#include "../library.c"

struct linux_dirent64
{
        p64 d_ino;
        p64 d_off;
        p16 d_reclen;
        p8 d_type;
        p8 d_name[];
};

positive2 term_size()
{
        positive2 size = {80, 24};

        struct
        {
                p16 rows;
                p16 cols;
                p16 xpixel;
                p16 ypixel;
        } data;

        if (!system_call_3(syscall(ioctl), 1, 0x5413, (positive)address_of data))
        {
                size.width = data.cols;
                size.height = data.rows;
        }

        return size;
}

fn sleep(timespec address_to time)
{
        system_call_3(syscall(nanosleep), (positive)time, 0, 0);
}

fn exit(b32 code)
{
        system_call_1(syscall(exit), code);
}

// The C side of the entry point. Everything here runs on a correctly aligned
// stack, so the compiler is free to vectorise as it likes.
//
// Only the top level asm below references this, which link time optimisation
// cannot see, so it has to be pinned or -flto plus --gc-sections drops it and
// the link fails on an undefined _start_c.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((used, externally_visible, noinline))
#else
__attribute__((used, noinline))
#endif
fn _start_c(p8 address_to stack_base)
{
        program_stack_base = stack_base;

        b32 result = main();

        exit(result);
}

// The kernel jumps straight to _start: it does not push a return address, so
// the stack is 16 byte aligned here rather than the "16 byte aligned minus the
// pushed return address" state the SysV ABI guarantees on entry to an ordinary
// function. Writing _start as a plain C function therefore ran the entire
// program 8 bytes out of phase, and the first aligned SSE store the compiler
// emitted faulted -- memory_fill vectorises into exactly that at -O2, which
// segfaulted the test suite partway through.
//
// Realigning in assembly before handing control to C is the only way to fix
// the phase, because a C function has already committed to its prologue.
#if X64
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "       xorq %rbp, %rbp\n"  // terminate the frame pointer chain
    "       movq %rsp, %rdi\n"  // hand the untouched stack base to C
    "       andq $-16, %rsp\n"  // realign for SSE
    "       call _start_c\n"
    "       hlt\n");
#elif ARM64
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "       mov x29, #0\n"  // terminate the frame pointer chain
    "       mov x30, #0\n"
    "       mov x0, sp\n"   // hand the untouched stack base to C
    "       and sp, x0, #-16\n"
    "       bl _start_c\n"
    "       brk #0\n");
#elif RISCV64
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "       li s0, 0\n"
    "       li ra, 0\n"
    "       mv a0, sp\n"
    "       andi sp, sp, -16\n"
    "       call _start_c\n"
    "       ebreak\n");
#else
#error "no _start defined for this architecture"
#endif

#endif