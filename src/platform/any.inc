#define MS_NONE 0
#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT 32
#define MS_MANDLOCK 64
#define MS_DIRSYNC 128
#define MS_NOSYMFOLLOW 256
#define MS_NOATIME 1024
#define MS_NODIRATIME 2048
#define MS_BIND 4096
#define MS_MOVE 8192
#define MS_REC 16384
#define MS_SILENT 32768
#define MS_POSIXACL (1 << 16)
#define MS_UNBINDABLE (1 << 17)
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE (1 << 19)
#define MS_SHARED (1 << 20)
#define MS_RELATIME (1 << 21)
#define MS_KERNMOUNT (1 << 22)
#define MS_I_VERSION (1 << 23)
#define MS_STRICTATIME (1 << 24)

#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define DT_FIFO 1
#define DT_SOCK 12
#define DT_CHR 2
#define DT_BLK 6

#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

// Logging ---------------------------------------------------

#define MAX_INPUT 4096

extern p8 log_writer_buffer[MAX_INPUT];
extern positive log_writer_buffer_length;

__asm__(
    ASM_BSS_OBJECT_BEGIN(log_writer_buffer, 64)
    ASM_ZERO(MAX_INPUT)
    ASM_OBJECT_END(log_writer_buffer)
    ASM_BSS_OBJECT_BEGIN(log_writer_buffer_length, 8)
    ".zero 8\n"
    ASM_OBJECT_END(log_writer_buffer_length)
);

fn log_direct(address_any data, positive length);
fn log_flush();
fn log(address_any data, positive length);
fn log_error(address_any data, positive length);

#define RUNTIME_TEXT_INNER(value) #value
#define RUNTIME_TEXT(value) RUNTIME_TEXT_INNER(value)

/*
        One buffer and one policy on all three machines:

          - a zero length asks string_length for the length;
          - a write that would take the buffer past 4096 flushes first;
          - fewer than 4096 bytes are buffered;
          - 4096 or more go directly to write(2).

        The additions and comparisons below are unsigned and wrap at the word
        boundary, just as positive did in the C body. Neither write result is
        inspected, and a flush clears the count after any non-empty write,
        including a short or failed one. Those details are observable and are
        deliberately unchanged.
*/
#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(log_direct)
    "mov %rsi, %rdx\n   mov %rdi, %rsi\n   mov $1, %edi\n"
    "mov $" RUNTIME_TEXT(syscall(write)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "mov log_writer_buffer_length(%rip), %rsi\n   test %rsi, %rsi\n"
    "jz .Llog_flush_x64_done\n   sub $8, %rsp\n"
    "lea log_writer_buffer(%rip), %rdi\n   call log_direct\n"
    "add $8, %rsp\n   movq $0, log_writer_buffer_length(%rip)\n"
    ".Llog_flush_x64_done:\n"
    ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_error)
    "push %r12\n   push %r13\n   sub $8, %rsp\n"
    "mov %rdi, %r12\n   mov %rsi, %r13\n   test %r13, %r13\n"
    "jnz .Llog_error_x64_have_length\n   call string_length\n   mov %rax, %r13\n"
    ".Llog_error_x64_have_length:\n"
    "call log_flush\n"
    "mov %r13, %rdx\n   mov %r12, %rsi\n   mov $2, %edi\n"
    "mov $" RUNTIME_TEXT(syscall(write)) ", %eax\n   syscall\n"
    "add $8, %rsp\n   pop %r13\n   pop %r12\n"
    ASM_RET
    ASM_END(log_error)

    ASM_FUNC(log)
    "push %r12\n   push %r13\n   push %r14\n"
    "mov %rdi, %r12\n   mov %rsi, %r13\n   test %r13, %r13\n"
    "jnz .Llog_x64_have_length\n   call string_length\n   mov %rax, %r13\n"
    ".Llog_x64_have_length:\n"
    "mov log_writer_buffer_length(%rip), %r14\n"
    "lea (%r13,%r14), %rax\n   cmp $4096, %rax\n"
    "jbe .Llog_x64_after_flush\n   call log_flush\n   xor %r14d, %r14d\n"
    ".Llog_x64_after_flush:\n"
    "cmp $4096, %r13\n   jae .Llog_x64_direct\n"
    "lea log_writer_buffer(%rip), %rdi\n   add %r14, %rdi\n"
    "mov %r12, %rsi\n   mov %r13, %rdx\n   call memory_copy\n"
    "add %r13, log_writer_buffer_length(%rip)\n"
    "jmp .Llog_x64_done\n"
    ".Llog_x64_direct:\n"
    "lea log_writer_buffer(%rip), %rdi\n"
    "mov log_writer_buffer_length(%rip), %rsi\n   call log_direct\n"
    "mov %r12, %rdi\n   mov %r13, %rsi\n   call log_direct\n"
    ".Llog_x64_done:\n"
    "pop %r14\n   pop %r13\n   pop %r12\n"
    ASM_RET
    ASM_END(log)
);
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(log_direct)
    "mov x2, x1\n   mov x1, x0\n   mov x0, #1\n"
    "movz " SYSCALL_NUMBER_REGISTER ", #((" RUNTIME_TEXT(syscall(write)) ") & 0xffff)\n"
    "movk " SYSCALL_NUMBER_REGISTER ", #(((" RUNTIME_TEXT(syscall(write)) ") >> 16) & 0xffff), lsl #16\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "adrp x9, log_writer_buffer_length\n"
    "ldr x1, [x9, :lo12:log_writer_buffer_length]\n"
    "cbz x1, .Llog_flush_arm64_done\n   str x30, [sp, #-16]!\n"
    "adrp x0, log_writer_buffer\n   add x0, x0, :lo12:log_writer_buffer\n"
    "bl log_direct\n   adrp x9, log_writer_buffer_length\n"
    "str xzr, [x9, :lo12:log_writer_buffer_length]\n"
    "ldr x30, [sp], #16\n"
    ".Llog_flush_arm64_done:\n"
    ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_error)
    "stp x19, x20, [sp, #-32]!\n   str x30, [sp, #16]\n"
    "mov x19, x0\n   mov x20, x1\n   cbnz x20, .Llog_error_arm64_have_length\n"
    "bl string_length\n   mov x20, x0\n"
    ".Llog_error_arm64_have_length:\n"
    "bl log_flush\n"
    "mov x2, x20\n   mov x1, x19\n   mov x0, #2\n"
    "movz " SYSCALL_NUMBER_REGISTER ", #((" RUNTIME_TEXT(syscall(write)) ") & 0xffff)\n"
    "movk " SYSCALL_NUMBER_REGISTER ", #(((" RUNTIME_TEXT(syscall(write)) ") >> 16) & 0xffff), lsl #16\n"
    SYSCALL_INSTRUCTION "\n"
    "ldr x30, [sp, #16]\n   ldp x19, x20, [sp], #32\n"
    ASM_RET
    ASM_END(log_error)

    ASM_FUNC(log)
    "stp x19, x20, [sp, #-32]!\n   stp x21, x30, [sp, #16]\n"
    "mov x19, x0\n   mov x20, x1\n   cbnz x20, .Llog_arm64_have_length\n"
    "bl string_length\n   mov x20, x0\n"
    ".Llog_arm64_have_length:\n"
    "adrp x9, log_writer_buffer_length\n"
    "ldr x21, [x9, :lo12:log_writer_buffer_length]\n"
    "add x9, x20, x21\n   cmp x9, #4096\n"
    "b.ls .Llog_arm64_after_flush\n   bl log_flush\n   mov x21, xzr\n"
    ".Llog_arm64_after_flush:\n"
    "cmp x20, #4096\n   b.hs .Llog_arm64_direct\n"
    "adrp x0, log_writer_buffer\n   add x0, x0, :lo12:log_writer_buffer\n"
    "add x0, x0, x21\n   mov x1, x19\n   mov x2, x20\n"
    "bl memory_copy\n   adrp x9, log_writer_buffer_length\n"
    "ldr x10, [x9, :lo12:log_writer_buffer_length]\n   add x10, x10, x20\n"
    "str x10, [x9, :lo12:log_writer_buffer_length]\n"
    "b .Llog_arm64_done\n"
    ".Llog_arm64_direct:\n"
    "adrp x0, log_writer_buffer\n   add x0, x0, :lo12:log_writer_buffer\n"
    "adrp x9, log_writer_buffer_length\n"
    "ldr x1, [x9, :lo12:log_writer_buffer_length]\n   bl log_direct\n"
    "mov x0, x19\n   mov x1, x20\n   bl log_direct\n"
    ".Llog_arm64_done:\n"
    "ldp x21, x30, [sp, #16]\n   ldp x19, x20, [sp], #32\n"
    ASM_RET
    ASM_END(log)
);
#elif RISCV64
__asm__(
    ASM_SECTION
    ASM_FUNC(log_direct)
    "mv a2, a1\n   mv a1, a0\n   li a0, 1\n"
    "li a7, " RUNTIME_TEXT(syscall(write)) "\n   ecall\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "lla t0, log_writer_buffer_length\n   ld a1, 0(t0)\n"
    "beqz a1, .Llog_flush_riscv64_done\n   addi sp, sp, -16\n"
    "sd ra, 8(sp)\n   lla a0, log_writer_buffer\n   call log_direct\n"
    "lla t0, log_writer_buffer_length\n   sd zero, 0(t0)\n"
    "ld ra, 8(sp)\n   addi sp, sp, 16\n"
    ".Llog_flush_riscv64_done:\n"
    ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_error)
    "addi sp, sp, -32\n   sd ra, 24(sp)\n   sd s0, 16(sp)\n"
    "sd s1, 8(sp)\n   mv s0, a0\n   mv s1, a1\n"
    "bnez s1, .Llog_error_riscv64_have_length\n   call string_length\n   mv s1, a0\n"
    ".Llog_error_riscv64_have_length:\n"
    "call log_flush\n"
    "mv a2, s1\n   mv a1, s0\n   li a0, 2\n"
    "li a7, " RUNTIME_TEXT(syscall(write)) "\n   ecall\n"
    "ld s1, 8(sp)\n   ld s0, 16(sp)\n   ld ra, 24(sp)\n"
    "addi sp, sp, 32\n"
    ASM_RET
    ASM_END(log_error)

    ASM_FUNC(log)
    "addi sp, sp, -32\n   sd ra, 24(sp)\n   sd s0, 16(sp)\n"
    "sd s1, 8(sp)\n   sd s2, 0(sp)\n   mv s0, a0\n   mv s1, a1\n"
    "bnez s1, .Llog_riscv64_have_length\n   call string_length\n   mv s1, a0\n"
    ".Llog_riscv64_have_length:\n"
    "lla t0, log_writer_buffer_length\n   ld s2, 0(t0)\n"
    "add t0, s1, s2\n   li t1, 4096\n"
    "bleu t0, t1, .Llog_riscv64_after_flush\n"
    "call log_flush\n   li s2, 0\n"
    ".Llog_riscv64_after_flush:\n"
    "li t0, 4096\n   bgeu s1, t0, .Llog_riscv64_direct\n"
    "lla a0, log_writer_buffer\n   add a0, a0, s2\n"
    "mv a1, s0\n   mv a2, s1\n   call memory_copy\n"
    "lla t0, log_writer_buffer_length\n   ld t1, 0(t0)\n"
    "add t1, t1, s1\n   sd t1, 0(t0)\n"
    "j .Llog_riscv64_done\n"
    ".Llog_riscv64_direct:\n"
    "lla a0, log_writer_buffer\n   lla t0, log_writer_buffer_length\n"
    "ld a1, 0(t0)\n   call log_direct\n"
    "mv a0, s0\n   mv a1, s1\n   call log_direct\n"
    ".Llog_riscv64_done:\n"
    "ld s2, 0(sp)\n   ld s1, 8(sp)\n   ld s0, 16(sp)\n"
    "ld ra, 24(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_END(log)
);
#else
#error "no logging implementation for this architecture"
#endif

#undef RUNTIME_TEXT
#undef RUNTIME_TEXT_INNER
