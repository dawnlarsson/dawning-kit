#
#       moonwater_ticks -- the machine's own free running counter.
#
#       One instruction on every architecture that has it, and no architecture
#       spells it the same way, which is the smallest honest example of what
#       .asm files here are for. There is no portable instruction to reach for
#       and no C that compiles to this, so the choice is a block per machine
#       or nothing.
#
#       What it returns is a hardware tick, not a nanosecond and not a cycle:
#       a monotonic count at a rate the platform picks, useful for measuring
#       one span against another and nothing else. The kernel's own
#       ktime_get() is the right answer for anything that needs a unit; this
#       is for the places already too hot to call it.
#
#       Declared in core.c, beside the code that calls it.
#
#include <linux/linkage.h>

        .text

SYM_FUNC_START(moonwater_ticks)

#> arch x86_64
        #
        #       rdtsc splits its answer across two 32 bit halves, which is
        #       older than the 64 bit registers it lands in. Writing eax and
        #       edx has already cleared the top of rax and rdx, so putting
        #       them together is a shift and an or.
        #
        #       Unserialized on purpose: lfence first would be the correct
        #       reading of "now" and costs more than the things this is meant
        #       to measure.
        #
        rdtsc
        shl     $32, %rdx
        or      %rdx, %rax

        #
        #       RET, not ret. Under a return thunk mitigation the kernel's
        #       macro is a jump to the thunk instead, and a bare ret in kernel
        #       assembly is the bug that leaves. It comes from asm/linkage.h,
        #       which linux/linkage.h above already pulled in.
        #
        RET

#> arch arm64
        #
        #       The virtual counter: fixed rate, readable at EL0 and EL1, and
        #       what the kernel's own arch_timer reads.
        #
        mrs     x0, cntvct_el0
        ret

#> arch riscv64
        #
        #       The time CSR is the fixed rate one and matches what the other
        #       two return; cycle would count clock ticks and change rate with
        #       frequency scaling.
        #
        csrr    a0, time
        ret

#> arch other
        #
        #       Reached only by an architecture added to glue without being
        #       given a counter here. Better a compile error naming the file
        #       than a function that returns whatever was in the return
        #       register.
        #
#error "moonwater_ticks: no tick counter for this architecture"

#> shared

SYM_FUNC_END(moonwater_ticks)
