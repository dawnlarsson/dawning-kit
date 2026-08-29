/*
        Experimental C standard library

        setjmp and longjmp: the one thing here that C cannot say

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_SETJMP
#define STANDARD_MODERN_C_PLATFORM_SETJMP

/*
        Why this is assembly and cannot be anything else.

        Every other structural routine in this tree is an algorithm wearing a
        libc name, and an algorithm is C. This one is not. jump_mark has to
        write down the callee-saved registers -- the ones the C abstract
        machine promises a call does not change -- and jump_to_mark has to put
        them back and then continue at an instruction the compiler chose,
        with a stack pointer the compiler chose. C has no expression that
        names rbx, or x19, or s0, and no statement that assigns to the stack
        pointer. The nearest thing C offers is a computed goto, and a computed
        goto cannot leave the function it was written in.

        So the register list is the whole of the correctness here, and it is
        different on all three machines. Getting it short is not a bug that
        shows up as a crash: it shows up as one local variable in the function
        that called jump_mark holding a stale value after the jump back, on
        one optimisation level, in one build. That is the failure this file
        exists to not have, so each list is written out from the procedure
        call standard rather than from memory of what libc saves.

        x86_64 System V, section 3.2.1: rbx, rbp, r12, r13, r14 and r15 belong
        to the caller, and so does rsp. The x87 control word and MXCSR are
        also caller-owned by the letter of that document, and neither is saved
        here, for the same reason glibc's longjmp does not restore them: a
        program that changed the rounding mode inside the region being jumped
        out of wanted the change, and the standard requires nothing either
        way.

        AArch64 AAPCS64, section 6.1.1: x19 through x28, the frame pointer x29
        and the link register x30, plus the stack pointer. The vector registers
        are the corner everybody forgets -- v8 through v15 are callee-saved,
        but only their low sixty four bits, which is exactly d8 through d15.
        Saving the full 128-bit q registers would be correct and twice the
        traffic for a guarantee nobody is given; saving nothing at all is the
        common bug, and it eats a double held across the jump.

        RISC-V, the calling convention chapter: s0 through s11, ra, sp, and
        fs0 through fs11 when the D extension is present, which the floor for
        this library says it is. Twelve integer and twelve float, the widest
        of the three lists by some way.

        Slot zero is the return address on every machine and slot one is the
        stack pointer, which is not an accident: the two things that make this
        a jump rather than a load are in the same place everywhere, and a
        debugger looking at a jump_state does not need to know which
        architecture wrote it.

        The stack pointer written down is the one the caller will have AFTER
        jump_mark returns, not the one at entry. The difference matters on
        x86_64, where the return address is below it: that slot is dead memory
        the moment jump_mark returns, and the calls made between the mark and
        the jump back will have overwritten it. Reading the return address
        back out of the stack is therefore wrong, and that is why it is copied
        into the state instead of being left where it lay.

        What is deliberately not here: the signal mask. POSIX setjmp is
        allowed to save it and glibc's does; sigsetjmp with a nonzero second
        argument must. Ours saves nothing, which makes it _setjmp exactly, and
        both names are attached to it below. A program that blocks a signal,
        jumps out, and expects the mask restored will not get that, and there
        is no sigprocmask call hidden in here to make it slow either.

        The mask half now exists and is next door. src/platform/signal.inc
        holds a sigsetjmp that saves the mask into slots 26 and 27 of the
        state below and then tail jumps into jump_mark, so the register lists
        here are the only ones in the library and are not copied there.
        Nothing in this file changed to make that work, and nothing here pays
        for it: setjmp and _setjmp still make no system call.

        Shadow stacks are not handled. A build with x86 CET shadow stack, or
        with AArch64 guarded control stacks, needs longjmp to unwind the
        shadow stack to match, and the indirect jump at the end of the x86_64
        body would fault. Nothing in this tree turns those on; a build that
        does needs this file revisited rather than trusted.
*/

/*
        Thirty two slots, where the widest machine needs twenty six.

        One size for all three keeps a jump_state a jump_state: a structure
        holding one can be declared in code that is compiled for every target
        without the structure changing shape. riscv64 is the widest at
        twenty six slots -- ra, sp, twelve integer and twelve float -- arm64
        needs twenty one and x86_64 eight. The spare six are not padding for
        its own sake; they are what a later addition, a signal mask among
        them, fits into without every caller's structure moving.

        A plain array of positive, so it decays to a pointer the way jmp_buf
        does and a caller writes jump_mark(state) rather than
        jump_mark(&state). Alignment is the element's eight bytes, which is
        what every store below wants: arm64's stp and riscv's fsd are natural
        at eight, and asking for sixteen through an attribute on an array
        typedef is a well known way to get an array whose elements are each
        sixteen bytes wide.
*/
#define JUMP_STATE_SLOTS 32

typedef positive jump_state[JUMP_STATE_SLOTS];

/*
        returns_twice is not decoration.

        Without it the compiler is entitled to assume control leaves this call
        once, which lets it keep a value live in a callee-saved register
        across the whole region, or sink a store past the call. Both are
        correct for an ordinary function and both are wrong here: the second
        arrival brings the register file back to what it was at the mark, and
        anything the compiler decided after that point is undone. gcc knows
        this about the name setjmp by itself and knows nothing about the name
        jump_mark, so the attribute is what tells it.

        It is still not a promise about automatic variables. An object local
        to the function that called jump_mark, not volatile, and modified
        between the mark and the jump back, has an indeterminate value
        afterwards -- that is C, not a limitation of this file, and volatile
        is the answer to it.

        jump_to_mark never returns, and saying so lets the caller's code end
        there rather than emitting a path that cannot be reached.
*/
b32 jump_mark(jump_state state) __attribute__((returns_twice));
fn jump_to_mark(jump_state state, b32 value) DEAD_END;

#if X64

/*
        Slot 0 return address, slot 1 the stack pointer after the return, then
        rbx rbp r12 r13 r14 r15 at 16 through 56.

        The value handed back is forced to one when the caller asked for zero,
        because the zero return is how the first arrival is told from the
        second and a longjmp that produced it would make the two
        indistinguishable. C requires the substitution; it does not require
        the caller to know it happened.

        The last instruction is an indirect jump and not a return, because the
        stack has already been moved to where it will be after the jump: there
        is no return address under rsp any more to pop. See the note above
        about shadow stacks, which is the one build where that distinction
        turns into a fault.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(jump_mark)
    "mov (%rsp), %rax\n   mov %rax, 0(%rdi)  # where the caller resumes\n"
    "lea 8(%rsp), %rax\n   mov %rax, 8(%rdi)  # its stack once this returns\n"
    "mov %rbx, 16(%rdi)\n   mov %rbp, 24(%rdi)\n"
    "mov %r12, 32(%rdi)\n   mov %r13, 40(%rdi)\n"
    "mov %r14, 48(%rdi)\n   mov %r15, 56(%rdi)\n"
    "xor %eax, %eax\n"
    ASM_RET
    ASM_END(jump_mark)

    ASM_FUNC(jump_to_mark)
    "mov %esi, %eax\n   test %eax, %eax\n   jne 1f\n   mov $1, %eax\n"
    "1:  mov 16(%rdi), %rbx\n   mov 24(%rdi), %rbp\n"
    "mov 32(%rdi), %r12\n   mov 40(%rdi), %r13\n"
    "mov 48(%rdi), %r14\n   mov 56(%rdi), %r15\n"
    "mov 8(%rdi), %rsp\n   jmp *0(%rdi)\n"
    ASM_END(jump_to_mark)
);

#elif ARM64

/*
        Slot 0 x30 and slot 1 the stack pointer, to keep the shared layout,
        then x19 through x28 at 16 through 88, the frame pointer x29 at 96,
        and d8 through d15 at 104 through 160. A hundred and sixty eight
        bytes, twenty one slots.

        Reading the stack pointer needs a scratch register because mov from sp
        is only encodable into a general register, and x2 is call-clobbered so
        it is free. On the way back sp is written last of the integer half for
        no reason the machine enforces -- it is written from x2 either way --
        but it keeps the one instruction that changes the frame next to the
        return.

        csinc is the whole of the zero substitution: with the flags from a
        compare against zero it hands back w1 when w1 is nonzero and wzr plus
        one when it is not, which is exactly the rule and no branch.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(jump_mark)
    "mov x2, sp\n"
    "stp x30, x2,  [x0, #0]     // resume address, then the stack\n"
    "stp x19, x20, [x0, #16]\n"
    "stp x21, x22, [x0, #32]\n"
    "stp x23, x24, [x0, #48]\n"
    "stp x25, x26, [x0, #64]\n"
    "stp x27, x28, [x0, #80]\n"
    "str x29,      [x0, #96]\n"
    "stp d8,  d9,  [x0, #104]\n"
    "stp d10, d11, [x0, #120]\n"
    "stp d12, d13, [x0, #136]\n"
    "stp d14, d15, [x0, #152]\n"
    "mov w0, wzr\n"
    ASM_RET
    ASM_END(jump_mark)

    ASM_FUNC(jump_to_mark)
    "ldp x30, x2,  [x0, #0]\n"
    "ldp x19, x20, [x0, #16]\n"
    "ldp x21, x22, [x0, #32]\n"
    "ldp x23, x24, [x0, #48]\n"
    "ldp x25, x26, [x0, #64]\n"
    "ldp x27, x28, [x0, #80]\n"
    "ldr x29,      [x0, #96]\n"
    "ldp d8,  d9,  [x0, #104]\n"
    "ldp d10, d11, [x0, #120]\n"
    "ldp d12, d13, [x0, #136]\n"
    "ldp d14, d15, [x0, #152]\n"
    "mov sp, x2\n"
    "cmp w1, #0\n   csinc w0, w1, wzr, ne\n"
    ASM_RET
    ASM_END(jump_to_mark)
);

#elif RISCV64

/*
        Slot 0 ra and slot 1 sp, then s0 through s11 at 16 through 104 and
        fs0 through fs11 at 112 through 200. Two hundred and eight bytes, the
        widest of the three, and the reason JUMP_STATE_SLOTS is what it is.

        Every offset is inside the twelve bit signed range a load or store
        takes directly, so nothing here needs an address built first -- which
        is why the state pointer is left in a0 untouched until the last two
        instructions, where it becomes the answer.

        s0 is the frame pointer in this ABI and s1 through s11 are ordinary
        callee-saved registers; they are one contiguous run in the store list
        because the numbering is contiguous, not because the roles are alike.
*/
__asm__(
    ASM_SECTION
    ASM_FUNC(jump_mark)
    "sd ra, 0(a0)    # where the caller resumes\n"
    "sd sp, 8(a0)\n"
    "sd s0, 16(a0)\n   sd s1, 24(a0)\n   sd s2, 32(a0)\n   sd s3, 40(a0)\n"
    "sd s4, 48(a0)\n   sd s5, 56(a0)\n   sd s6, 64(a0)\n   sd s7, 72(a0)\n"
    "sd s8, 80(a0)\n   sd s9, 88(a0)\n   sd s10, 96(a0)\n   sd s11, 104(a0)\n"
    "fsd fs0, 112(a0)\n   fsd fs1, 120(a0)\n   fsd fs2, 128(a0)\n"
    "fsd fs3, 136(a0)\n   fsd fs4, 144(a0)\n   fsd fs5, 152(a0)\n"
    "fsd fs6, 160(a0)\n   fsd fs7, 168(a0)\n   fsd fs8, 176(a0)\n"
    "fsd fs9, 184(a0)\n   fsd fs10, 192(a0)\n   fsd fs11, 200(a0)\n"
    "li a0, 0\n"
    ASM_RET
    ASM_END(jump_mark)

    ASM_FUNC(jump_to_mark)
    "ld ra, 0(a0)\n"
    "ld s0, 16(a0)\n   ld s1, 24(a0)\n   ld s2, 32(a0)\n   ld s3, 40(a0)\n"
    "ld s4, 48(a0)\n   ld s5, 56(a0)\n   ld s6, 64(a0)\n   ld s7, 72(a0)\n"
    "ld s8, 80(a0)\n   ld s9, 88(a0)\n   ld s10, 96(a0)\n   ld s11, 104(a0)\n"
    "fld fs0, 112(a0)\n   fld fs1, 120(a0)\n   fld fs2, 128(a0)\n"
    "fld fs3, 136(a0)\n   fld fs4, 144(a0)\n   fld fs5, 152(a0)\n"
    "fld fs6, 160(a0)\n   fld fs7, 168(a0)\n   fld fs8, 176(a0)\n"
    "fld fs9, 184(a0)\n   fld fs10, 192(a0)\n   fld fs11, 200(a0)\n"
    "ld sp, 8(a0)    # last, so every other load still had its base\n"
    "mv a0, a1\n   bnez a1, 1f\n   li a0, 1\n"
    "1:\n"
    ASM_RET
    ASM_END(jump_to_mark)
);

#endif

/*
        The libc spellings, as second labels on the same address.

        setjmp and _setjmp differ in glibc only in whether the signal mask
        travels with the jump, and since nothing here carries a mask the two
        are the same code and get the same address. That is a real difference
        in behaviour from glibc for the unprefixed name, written down in full
        in the note at the top of this file rather than hidden behind a name
        that looks familiar.

        sigsetjmp and siglongjmp are not here and are not missing. They were
        absent while there was no sigprocmask to build them out of, on the
        grounds that a routine taking the second argument and ignoring it
        would be a promise broken quietly. src/standard/signal.c supplies the
        mask, and src/platform/signal.inc supplies the two names beside the
        restorer trampoline that lives there for the same reason -- both are
        per-architecture instructions, and one file of those is enough.
*/
__asm__(
    ASM_ALIAS(setjmp,   jump_mark)
    ASM_ALIAS(_setjmp,  jump_mark)
    ASM_ALIAS(longjmp,  jump_to_mark)
    ASM_ALIAS(_longjmp, jump_to_mark)
);

#endif // STANDARD_MODERN_C_PLATFORM_SETJMP
