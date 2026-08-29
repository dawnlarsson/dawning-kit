/*
        Experimental C standard library

        The two pieces of the signal family that have to be instructions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_SIGNAL
#define STANDARD_MODERN_C_PLATFORM_SIGNAL

/*
        Why there is any assembly here at all, and why there is so little.

        Everything else the signal family does is a system call with a
        structure copied into the shape the kernel wants, which is C. Two
        things are not.

        The first is where a handler goes when it returns. Linux does not
        arrange for a signal handler to return the way a function returns: it
        builds a frame on the stack, points the program counter at the
        handler, and leaves a return address that has to lead to a
        rt_sigreturn system call, because only that call knows how to put the
        interrupted register file back. On x86_64 the kernel does not supply
        that address -- sa_restorer is a field the caller fills in and
        SA_RESTORER is the flag that says it did -- so a program that installs
        a handler without one gets a return into whatever was in that field,
        which is a fault at best. glibc supplies a two instruction restorer
        and so does this file.

        arm64 and riscv64 do not need one and are given none. Both kernels
        write the return address themselves, pointing it at a trampoline the
        vDSO carries: arm64's setup_rt_frame sets x30 and riscv's sets ra,
        and neither is asked for a restorer. arm64 does define SA_RESTORER and
        will honour one if the flag is set, and riscv64 does not define the
        flag at all -- its uapi has no sa_restorer field, so the bit would be
        setting an unknown flag in a structure whose third word is the mask.
        That difference is not cosmetic and it is the reason SA_RESTORER is
        set on exactly one of the three below rather than on all of them.

        The second is sigsetjmp, and the reason is subtler. sigsetjmp cannot
        be a C function that saves the mask and then calls jump_mark, because
        jump_mark writes down the stack pointer its own caller will have after
        it returns -- which would be a pointer into the C wrapper's frame.
        That frame is dead the instant the wrapper returns. A later
        siglongjmp would restore a stack pointer to memory that other calls
        have since overwritten, reload the callee-saved registers out of it,
        and continue with a register file made of whatever was last pushed
        there. It does not fail every time, which is the worst kind.

        So sigsetjmp is a stub that saves the mask and then TAIL jumps to
        jump_mark with the stack exactly as it was on entry. jump_mark then
        records sigsetjmp's own caller, which is what it must record, and the
        register lists in setjmp.inc are not copied here or anywhere else.
        There is one save-the-registers routine in this library and this file
        does not become a second one.
*/

/*
        Where the mask goes, and why it goes in the same array.

        setjmp.inc reserves thirty two slots for a jump_state and its comment
        says why: riscv64 is the widest at twenty six slots, and the spare six
        are for "a later addition, a signal mask among them". This is that
        addition. Slot 26 records whether a mask was asked for at all and slot
        27 holds it, so a sigjmp_buf and a jmp_buf are the same object and a
        program that mixes the two spellings gets a compile error rather than
        a structure of the wrong size.

        The mask stored is the kernel's eight bytes and not the hundred and
        twenty eight glibc declares, because eight is all sixty four signals
        need and all the system call will read.
*/
#define SIGNAL_JUMP_SAVED 26
#define SIGNAL_JUMP_MASK 27

/*
        The two halves of the stub, one in C and one in instructions.

        signal_jump_save is ordinary C -- it reads the mask with a system call
        and writes two slots -- and it is deliberately not static: the stub
        below calls it by name, and a call written inside a string is
        invisible to the compiler, so a static definition would be discarded
        as unused before the assembler ever saw the reference. That is the
        same reason src/test/run's comment gives for keeping -flto off.  The
        normal program build does use LTO, which can internalise even an
        unreferenced global, so KEEP is part of this declaration's ABI: the
        assembler reference is real even though the compiler cannot see it.
*/
KEEP fn signal_jump_save(jump_state state, b32 save_mask);

b32 signal_jump_mark(jump_state state, b32 save_mask) __attribute__((returns_twice));
fn signal_jump_to_mark(jump_state state, b32 value) DEAD_END;

#if X64

/*
        The restorer, which is the whole of what x86_64 needs and the other
        two do not.

        Two instructions and no stack frame, because there is no stack frame
        to have: the kernel arranged for the handler to return here, and what
        is under the stack pointer at this moment is the signal frame the
        kernel built, which is exactly what rt_sigreturn expects to find. A
        prologue of any kind would move the stack pointer off it.

        The number is taken through MOONWATER_NUMBER rather than written as
        fifteen, so it stays whatever the syscall table says it is.

        ASM_FUNC puts an endbr64 at the top. The kernel reaches this with a
        return rather than an indirect jump, so indirect branch tracking does
        not require the marker, but a build with a shadow stack would need
        more than a marker here and setjmp.inc's closing note already says
        that the indirect jump at the end of jump_to_mark is the other place
        such a build has to be revisited.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(signal_return_trampoline)
    "mov $" MOONWATER_NUMBER(syscall(rt_sigreturn)) ", %eax\n"
    "syscall\n"
    ASM_END(signal_return_trampoline)
);

fn signal_return_trampoline(void);

/*
        sigsetjmp.

        Eight bytes of stack and one call. rdi is the state and esi is the
        flag, which is already the argument order signal_jump_save wants, so
        nothing is moved into place; rdi is pushed only because it is
        call-clobbered and jump_mark needs it back.

        The alignment is the part worth checking rather than reading past. At
        entry the stack pointer is eight modulo sixteen, because the call that
        arrived here pushed a return address onto an aligned stack. The push
        makes it zero modulo sixteen, which is what the ABI requires
        immediately before a call. There is no second adjustment and adding
        one would break it.

        After the pop the stack pointer is byte for byte what it was on
        entry, so jump_mark reads the original caller's return address out of
        (%rsp) and computes the original caller's stack from it. The jump
        rather than a call is the whole point: jump_mark returns to
        sigsetjmp's caller, not to sigsetjmp.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(signal_jump_mark)
    "push %rdi\n"
    "call signal_jump_save\n"
    "pop %rdi\n"
    "jmp jump_mark\n"
    ASM_END(signal_jump_mark)
);

#elif ARM64

/*
        No restorer. arm64's setup_rt_frame writes x30 itself, pointing at
        the vDSO's sigreturn trampoline, unless SA_RESTORER is set -- and the
        C beside this does not set it, so the kernel's own is used.

        The stub saves x0 and the link register in one store pair and gets
        both back in one load pair, which is also what keeps the stack sixteen
        byte aligned across the call without an instruction spent on it.

        b rather than bl, so x30 still holds the original caller's return
        address when jump_mark stores it, and sp still holds the original
        caller's stack when jump_mark reads it.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(signal_jump_mark)
    "stp x0, x30, [sp, #-16]!\n"
    "bl signal_jump_save\n"
    "ldp x0, x30, [sp], #16\n"
    "b jump_mark\n"
    ASM_END(signal_jump_mark)
);

#elif RISCV64

/*
        No restorer, and no field to put one in. riscv64's uapi does not
        define SA_RESTORER and its struct sigaction is the asm-generic three
        word one -- handler, flags, mask -- so the bit that means "restorer
        supplied" on the other two machines would here be an unrecognised
        flag sitting beside a mask at a different offset. The kernel writes ra
        to the vDSO trampoline in setup_rt_frame, which is the whole
        arrangement.

        Sixteen bytes of stack rather than the eight the two saved registers
        need, because the ABI wants the stack sixteen byte aligned at a call
        and this is the cheapest way to keep it so.

        tail rather than j, so the branch reaches wherever jump_mark was
        placed rather than only within a megabyte of here. It clobbers t1,
        which is caller-saved and which jump_mark does not read.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(signal_jump_mark)
    "addi sp, sp, -16\n"
    "sd a0, 0(sp)\n"
    "sd ra, 8(sp)\n"
    "call signal_jump_save\n"
    "ld a0, 0(sp)\n"
    "ld ra, 8(sp)\n"
    "addi sp, sp, 16\n"
    "tail jump_mark\n"
    ASM_END(signal_jump_mark)
);

#endif

#endif // STANDARD_MODERN_C_PLATFORM_SIGNAL
