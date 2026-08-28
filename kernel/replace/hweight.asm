#
#       __sw_hweight32 / __sw_hweight64 -- population count.
#
#       arch/x86/lib/hweight.S is about a hundred and thirty lines of SWAR bit
#       twiddling -- mask, shift, add, multiply, shift -- roughly twenty five
#       instructions to count the bits in a word. It exists for processors
#       without POPCNT, and the caller reaches it through
#
#           ALTERNATIVE("call __sw_hweight64",
#                       "popcntq %[val], %[cnt]", X86_FEATURE_POPCNT)
#
#       so on anything with the instruction, all 332 call sites in the image
#       are rewritten to a single popcntq when alternatives are applied.
#
#       That fallback cannot run here. This kernel is compiled -march=x86-64-v2,
#       whose baseline includes POPCNT, and gcc already emits the instruction in
#       ordinary C. A processor that reached this code would have died on an
#       illegal opcode long before arriving.
#
#include <linux/export.h>
#include <linux/linkage.h>
#include <linux/objtool.h>

        .text

#> arch x86_64
        #
        #       ANNOTATE_NOENDBR is kept from the original. It is a no-op
        #       while X86_KERNEL_IBT is off, which it is here, and it is what
        #       objtool wants the moment anybody turns IBT back on.
        #
SYM_FUNC_START(__sw_hweight32)
        ANNOTATE_NOENDBR
        popcntl %edi, %eax
        RET
SYM_FUNC_END(__sw_hweight32)
EXPORT_SYMBOL(__sw_hweight32)

SYM_FUNC_START(__sw_hweight64)
        ANNOTATE_NOENDBR
        popcntq %rdi, %rax
        RET
SYM_FUNC_END(__sw_hweight64)
EXPORT_SYMBOL(__sw_hweight64)

#> arch other
        #
        #       Deliberate, and not an omission.
        #
        #       Only x86 keeps its hweight in assembly. arm64 has no population
        #       count for general registers at all -- cnt is a SIMD instruction
        #       -- and reaches these symbols through the generic C in
        #       lib/hweight.c. riscv has cpop under Zbb and falls back to the
        #       same C file.
        #
#error "hweight.asm replaces arch/x86/lib/hweight.S and has no meaning on this architecture"
