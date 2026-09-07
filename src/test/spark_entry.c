#include "../compiler_memory.c"

#if X64
#define SPARK_TEST_TEXT_(value) #value
#define SPARK_TEST_TEXT(value) SPARK_TEST_TEXT_(value)
/* Arrive at the real _start with the original Linux stack and a simulated
   loader handoff. The zero-flag case proves CPUID was actually bypassed. */
__asm__(
    ASM_SECTION
    ASM_FUNC(spark_entry_probe)
    "mov 16(%rsp), %rax\n   movzbl (%rax), %eax\n   sub $48, %eax\n"
    "cmp $8, %eax\n   ja 1f\n"
    "lea spark_entry_words(%rip), %rcx\n   mov (%rcx,%rax,8), %r13\n"
    "movabs $" SPARK_TEST_TEXT(SPARK_START_MAGIC) ", %r12\n"
    "mov $123456789, %r14\n   cmp $7, %eax\n   jne 2f\n"
    "1: xor %r12d, %r12d\n"
    "2: jmp _start\n"
    ASM_END(spark_entry_probe)
    ASM_RODATA_OBJECT_BEGIN(spark_entry_words, 8)
    ".quad 0, 1, 257, 16777216, 16777217, 16777473, 16843009, 0, 0xabcdef1201010101\n"
    ASM_OBJECT_END(spark_entry_words)
);

b32 main(void)
{
        static positive words[] = {0, 1, 257, 16777216, 16777217,
                                    16777473, 16843009, 0, 16843009};
        positive mode = program_argument(1)[0] - '0';
        positive got = cpu_has_avx2 | ((positive)cpu_has_avx512 << 8) |
            ((positive)cpu_has_avx512_vbmi << 16) | ((positive)cpu_has_fma << 24);
        if (mode == 7)
        {
                cpu_has_avx2 = cpu_has_avx512 = cpu_has_avx512_vbmi = cpu_has_fma = 0;
                moonwater_cpu_detect();
                positive detected = cpu_has_avx2 | ((positive)cpu_has_avx512 << 8) |
                    ((positive)cpu_has_avx512_vbmi << 16) | ((positive)cpu_has_fma << 24);
                return got != detected || program_initial_identity() !=
                    (positive)system_call_1(syscall(getpid), 0);
        }
        // Do not execute instructions advertised by a synthetic handoff:
        // scalar-only CPUs can still verify every publication combination.
        return mode >= array_count(words) || got != words[mode] ||
               program_initial_identity() != 123456789 ||
               program_argument_count() != 2 || !program_environment_list();
}
#endif
