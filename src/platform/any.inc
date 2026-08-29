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
extern p8 log_writer_failed;

__asm__(
    ASM_BSS_OBJECT_BEGIN(log_writer_buffer, 64)
    ASM_ZERO(MAX_INPUT)
    ASM_OBJECT_END(log_writer_buffer)
    ASM_BSS_OBJECT_BEGIN(log_writer_buffer_length, 8)
    ".zero 8\n"
    ASM_OBJECT_END(log_writer_buffer_length)
    ASM_BSS_OBJECT_BEGIN(log_writer_failed, 1)
    ".zero 1\n"
    ASM_OBJECT_END(log_writer_failed)
);

fn log_direct(address_any data, positive length);
fn log_flush();
fn log(address_any data, positive length);
fn log_error(address_any data, positive length);
fn log_failure_reset();
bool log_failed();
b32 buffered_flush(positive handle, p8 address_to buffer, positive address_to used);
b32 buffered_write(positive handle, p8 address_to buffer, positive capacity,
                   positive address_to used, address_any data, positive length);
b32 buffered_write_deferred_equal(positive handle, p8 address_to buffer,
                                  positive capacity, positive address_to used,
                                  address_any data, positive length);
b32 buffered_write_byte(positive handle, p8 address_to buffer, positive capacity,
                        positive address_to used, p8 byte);

#define RUNTIME_TEXT_INNER(value) #value
#define RUNTIME_TEXT(value) RUNTIME_TEXT_INNER(value)

/*
        Shared buffered output at the architecture floor.  The regular entry
        writes runs at least as large as the buffer directly; the deferred-
        equal entry keeps an exact fit buffered for text's historical policy.
        Every entry returns one only when all bytes it accepted were written or
        retained. Flush always clears the count, including after a failed
        write, so a caller can report the failure without writing the same
        prefix twice on a later flush.
*/
#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(buffered_flush)
    "sub $24, %rsp\n   mov %rdx, 0(%rsp)\n   mov (%rdx), %rdx\n"
    "test %rdx, %rdx\n   jz .Lbuffered_flush_x64_empty\n"
    "mov %rdx, 8(%rsp)\n   call system_write_all\n"
    "cmp 8(%rsp), %rax\n   sete %al\n   movzbl %al, %eax\n"
    "mov 0(%rsp), %rdx\n   movq $0, (%rdx)\n"
    ".Lbuffered_flush_x64_done:\n   add $24, %rsp\n"
    ASM_RET
    ".Lbuffered_flush_x64_empty:\n   mov $1, %eax\n   jmp .Lbuffered_flush_x64_done\n"
    ASM_END(buffered_flush)

    ASM_FUNC(buffered_write)
    "test %r9, %r9\n   jz .Lbuffered_write_x64_empty\n"
    "xor %eax, %eax\n   jmp buffered_write_core\n"
    ".Lbuffered_write_x64_empty:\n   mov $1, %eax\n" ASM_RET
    ASM_END(buffered_write)
    ASM_FUNC(buffered_write_deferred_equal)
    "test %r9, %r9\n   jz .Lbuffered_write_deferred_x64_empty\n"
    "mov $1, %eax\n   jmp buffered_write_core\n"
    ".Lbuffered_write_deferred_x64_empty:\n   mov $1, %eax\n" ASM_RET
    ASM_END(buffered_write_deferred_equal)
    ASM_LOCAL_FUNC(buffered_write_core)
    "test %eax, %eax\n   jnz .Lbuffered_write_x64_deferred\n"
    "cmp %rdx, %r9\n   jae .Lbuffered_write_x64_direct_entry\n"
    "jmp .Lbuffered_write_x64_buffer_entry\n"
    ".Lbuffered_write_x64_deferred:\n   cmp %rdx, %r9\n"
    "ja .Lbuffered_write_x64_direct_entry\n"
    ".Lbuffered_write_x64_buffer_entry:\n"
    "mov %rdx, %rax\n   sub %r9, %rax\n   mov (%rcx), %r10\n"
    "cmp %rax, %r10\n   ja .Lbuffered_write_x64_flush_first\n"
    "sub $24, %rsp\n   mov %rcx, 0(%rsp)\n   mov %r9, 8(%rsp)\n"
    "lea (%rsi,%r10), %rdi\n   mov %r8, %rsi\n   mov %r9, %rdx\n"
    "call memory_copy\n   mov 0(%rsp), %rcx\n   mov 8(%rsp), %r9\n"
    "add %r9, (%rcx)\n   mov $1, %eax\n   add $24, %rsp\n"
    ASM_RET
    ".Lbuffered_write_x64_flush_first:\n   sub $40, %rsp\n"
    "mov %rsi, 0(%rsp)\n   mov %rcx, 8(%rsp)\n"
    "mov %r8, 16(%rsp)\n   mov %r9, 24(%rsp)\n"
    "mov %rcx, %rdx\n   call buffered_flush\n"
    "test %eax, %eax\n   jz .Lbuffered_write_x64_leave_40\n"
    "mov 0(%rsp), %rdi\n   mov 16(%rsp), %rsi\n   mov 24(%rsp), %rdx\n"
    "call memory_copy\n   mov 8(%rsp), %rcx\n   mov 24(%rsp), %r9\n"
    "add %r9, (%rcx)\n   mov $1, %eax\n"
    ".Lbuffered_write_x64_leave_40:\n   add $40, %rsp\n"
    ASM_RET
    ".Lbuffered_write_x64_direct_entry:\n"
    "sub $40, %rsp\n   mov %rdi, 0(%rsp)\n   mov %r8, 8(%rsp)\n"
    "mov %r9, 16(%rsp)\n   mov %rcx, %rdx\n   call buffered_flush\n"
    "test %eax, %eax\n   jz .Lbuffered_write_x64_leave_40\n"
    "mov 0(%rsp), %rdi\n   mov 8(%rsp), %rsi\n   mov 16(%rsp), %rdx\n"
    "call system_write_all\n   cmp 16(%rsp), %rax\n   sete %al\n   movzbl %al, %eax\n"
    "add $40, %rsp\n"
    ASM_RET
    ASM_LOCAL_END(buffered_write_core)

    ASM_FUNC(buffered_write_byte)
    "test %rdx, %rdx\n   jz .Lbuffered_byte_x64_direct\n"
    "mov (%rcx), %rax\n   cmp %rdx, %rax\n   jae .Lbuffered_byte_x64_flush\n"
    "mov %r8b, (%rsi,%rax)\n   incq (%rcx)\n"
    ".Lbuffered_byte_x64_success:\n   mov $1, %eax\n"
    ASM_RET
    ".Lbuffered_byte_x64_direct:\n   sub $8, %rsp\n   mov %r8b, (%rsp)\n"
    "mov %rsp, %rsi\n   mov $1, %edx\n   call system_write_all\n"
    "cmp $1, %rax\n   sete %al\n   movzbl %al, %eax\n   add $8, %rsp\n"
    ASM_RET
    ".Lbuffered_byte_x64_flush:\n   sub $40, %rsp\n"
    "mov %rsi, 0(%rsp)\n   mov %rcx, 8(%rsp)\n   mov %r8, 16(%rsp)\n"
    "mov %rcx, %rdx\n   call buffered_flush\n"
    "test %eax, %eax\n   jz .Lbuffered_byte_x64_failed\n"
    "mov 0(%rsp), %rsi\n   mov 8(%rsp), %rcx\n   mov 16(%rsp), %r8\n"
    "mov %r8b, (%rsi)\n   movq $1, (%rcx)\n   mov $1, %eax\n"
    ".Lbuffered_byte_x64_failed:\n   add $40, %rsp\n"
    ASM_RET
    ASM_END(buffered_write_byte)

    ASM_FUNC(log_direct)
    "sub $24, %rsp\n   mov %rsi, 0(%rsp)\n"
    "mov %rsi, %rdx\n   mov %rdi, %rsi\n   mov $1, %edi\n"
    "call system_write_all\n   cmp 0(%rsp), %rax\n"
    "je .Llog_direct_x64_done\n   movb $1, log_writer_failed(%rip)\n"
    ".Llog_direct_x64_done:\n   add $24, %rsp\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "sub $8, %rsp\n"
    "mov $1, %edi\n   lea log_writer_buffer(%rip), %rsi\n"
    "lea log_writer_buffer_length(%rip), %rdx\n   call buffered_flush\n"
    "test %eax, %eax\n   jnz .Llog_flush_x64_done\n"
    "movb $1, log_writer_failed(%rip)\n"
    ".Llog_flush_x64_done:\n   add $8, %rsp\n" ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_failure_reset)
    "movb $0, log_writer_failed(%rip)\n" ASM_RET
    ASM_END(log_failure_reset)

    ASM_FUNC(log_failed)
    "movzbl log_writer_failed(%rip), %eax\n" ASM_RET
    ASM_END(log_failed)

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
    "test %rsi, %rsi\n   jnz .Llog_x64_have_length\n"
    "push %rdi\n   call string_length\n   mov %rax, %rsi\n   pop %rdi\n"
    ".Llog_x64_have_length:\n   mov %rsi, %r9\n   mov %rdi, %r8\n"
    "lea log_writer_buffer_length(%rip), %rcx\n   mov $4096, %edx\n"
    "lea log_writer_buffer(%rip), %rsi\n   mov $1, %edi\n"
    "sub $8, %rsp\n   call buffered_write\n"
    "test %eax, %eax\n   jnz .Llog_x64_done\n"
    "movb $1, log_writer_failed(%rip)\n"
    ".Llog_x64_done:\n   add $8, %rsp\n" ASM_RET
    ASM_END(log)
);
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(buffered_flush)
    "ldr x3, [x2]\n   cbz x3, .Lbuffered_flush_arm64_empty\n"
    "stp x2, x3, [sp, #-32]!\n   str x30, [sp, #16]\n   mov x2, x3\n"
    "bl system_write_all\n   ldp x2, x3, [sp]\n   ldr x30, [sp, #16]\n"
    "cmp x0, x3\n   cset w0, eq\n   str xzr, [x2]\n   add sp, sp, #32\n"
    ASM_RET
    ".Lbuffered_flush_arm64_empty:\n   mov w0, #1\n" ASM_RET
    ASM_END(buffered_flush)

    ASM_FUNC(buffered_write)
    "cbz x5, .Lbuffered_write_arm64_empty\n"
    "mov w6, wzr\n   b buffered_write_core\n"
    ".Lbuffered_write_arm64_empty:\n   mov w0, #1\n" ASM_RET
    ASM_END(buffered_write)
    ASM_FUNC(buffered_write_deferred_equal)
    "cbz x5, .Lbuffered_write_deferred_arm64_empty\n"
    "mov w6, #1\n   b buffered_write_core\n"
    ".Lbuffered_write_deferred_arm64_empty:\n   mov w0, #1\n" ASM_RET
    ASM_END(buffered_write_deferred_equal)
    ASM_LOCAL_FUNC(buffered_write_core)
    "cbnz w6, .Lbuffered_write_arm64_deferred\n"
    "cmp x5, x2\n   b.hs .Lbuffered_write_arm64_direct_entry\n"
    "b .Lbuffered_write_arm64_buffer_entry\n"
    ".Lbuffered_write_arm64_deferred:\n   cmp x5, x2\n"
    "b.hi .Lbuffered_write_arm64_direct_entry\n"
    ".Lbuffered_write_arm64_buffer_entry:\n"
    "sub x6, x2, x5\n   ldr x7, [x3]\n   cmp x7, x6\n"
    "b.hi .Lbuffered_write_arm64_flush_first\n"
    "stp x3, x5, [sp, #-32]!\n   str x30, [sp, #16]\n"
    "add x0, x1, x7\n   mov x1, x4\n   mov x2, x5\n"
    "bl memory_copy\n   ldp x3, x5, [sp]\n   ldr x30, [sp, #16]\n"
    "ldr x7, [x3]\n   add x7, x7, x5\n   str x7, [x3]\n"
    "mov w0, #1\n   add sp, sp, #32\n"
    ASM_RET
    ".Lbuffered_write_arm64_flush_first:\n"
    "sub sp, sp, #48\n   stp x1, x3, [sp]\n"
    "stp x4, x5, [sp, #16]\n   str x30, [sp, #32]\n"
    "mov x2, x3\n   bl buffered_flush\n"
    "cbz w0, .Lbuffered_write_arm64_leave_48\n"
    "ldp x0, x3, [sp]\n   ldp x1, x2, [sp, #16]\n"
    "bl memory_copy\n   ldr x3, [sp, #8]\n   ldr x5, [sp, #24]\n"
    "ldr x7, [x3]\n   add x7, x7, x5\n   str x7, [x3]\n"
    "mov w0, #1\n"
    ".Lbuffered_write_arm64_leave_48:\n   ldr x30, [sp, #32]\n   add sp, sp, #48\n"
    ASM_RET
    ".Lbuffered_write_arm64_direct_entry:\n"
    "sub sp, sp, #32\n   stp x0, x4, [sp]\n"
    "stp x5, x30, [sp, #16]\n   mov x2, x3\n"
    "bl buffered_flush\n   cbz w0, .Lbuffered_write_arm64_direct_failed\n"
    "ldp x0, x1, [sp]\n   ldr x2, [sp, #16]\n"
    "bl system_write_all\n   ldr x2, [sp, #16]\n   cmp x0, x2\n   cset w0, eq\n"
    ".Lbuffered_write_arm64_direct_failed:\n   ldr x30, [sp, #24]\n   add sp, sp, #32\n"
    ASM_RET
    ASM_LOCAL_END(buffered_write_core)

    ASM_FUNC(buffered_write_byte)
    "cbz x2, .Lbuffered_byte_arm64_direct\n   ldr x5, [x3]\n"
    "cmp x5, x2\n   b.hs .Lbuffered_byte_arm64_flush\n"
    "strb w4, [x1, x5]\n   add x5, x5, #1\n   str x5, [x3]\n"
    ".Lbuffered_byte_arm64_success:\n   mov w0, #1\n"
    ASM_RET
    ".Lbuffered_byte_arm64_direct:\n"
    "stp x29, x30, [sp, #-32]!\n   strb w4, [sp, #16]\n"
    "add x1, sp, #16\n   mov x2, #1\n   bl system_write_all\n"
    "cmp x0, #1\n   cset w0, eq\n   ldp x29, x30, [sp], #32\n"
    ASM_RET
    ".Lbuffered_byte_arm64_flush:\n"
    "stp x1, x3, [sp, #-32]!\n   stp x4, x30, [sp, #16]\n"
    "mov x2, x3\n   bl buffered_flush\n   ldp x1, x3, [sp]\n"
    "cbz w0, .Lbuffered_byte_arm64_failed\n"
    "ldp x4, x30, [sp, #16]\n   strb w4, [x1]\n"
    "mov x5, #1\n   str x5, [x3]\n   mov w0, #1\n"
    ".Lbuffered_byte_arm64_failed:\n   ldr x30, [sp, #24]\n   add sp, sp, #32\n"
    ASM_RET
    ASM_END(buffered_write_byte)

    ASM_FUNC(log_direct)
    "stp x1, x30, [sp, #-16]!\n"
    "mov x2, x1\n   mov x1, x0\n   mov x0, #1\n"
    "bl system_write_all\n   ldr x1, [sp]\n   cmp x0, x1\n"
    "b.eq .Llog_direct_arm64_done\n   adrp x2, log_writer_failed\n"
    "add x2, x2, :lo12:log_writer_failed\n   mov w1, #1\n   strb w1, [x2]\n"
    ".Llog_direct_arm64_done:\n   ldr x30, [sp, #8]\n   add sp, sp, #16\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "str x30, [sp, #-16]!\n"
    "mov x0, #1\n   adrp x1, log_writer_buffer\n"
    "add x1, x1, :lo12:log_writer_buffer\n"
    "adrp x2, log_writer_buffer_length\n"
    "add x2, x2, :lo12:log_writer_buffer_length\n"
    "bl buffered_flush\n   cbnz w0, .Llog_flush_arm64_done\n"
    "adrp x1, log_writer_failed\n   add x1, x1, :lo12:log_writer_failed\n"
    "mov w2, #1\n   strb w2, [x1]\n"
    ".Llog_flush_arm64_done:\n   ldr x30, [sp], #16\n" ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_failure_reset)
    "adrp x0, log_writer_failed\n   add x0, x0, :lo12:log_writer_failed\n"
    "strb wzr, [x0]\n" ASM_RET
    ASM_END(log_failure_reset)

    ASM_FUNC(log_failed)
    "adrp x0, log_writer_failed\n   add x0, x0, :lo12:log_writer_failed\n"
    "ldrb w0, [x0]\n" ASM_RET
    ASM_END(log_failed)

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
    "cbnz x1, .Llog_arm64_have_length\n"
    "stp x19, x30, [sp, #-16]!\n   mov x19, x0\n"
    "bl string_length\n   mov x1, x0\n   mov x0, x19\n"
    "ldp x19, x30, [sp], #16\n"
    ".Llog_arm64_have_length:\n   mov x5, x1\n   mov x4, x0\n"
    "adrp x3, log_writer_buffer_length\n"
    "add x3, x3, :lo12:log_writer_buffer_length\n"
    "mov x2, #4096\n   adrp x1, log_writer_buffer\n"
    "add x1, x1, :lo12:log_writer_buffer\n   mov x0, #1\n"
    "str x30, [sp, #-16]!\n   bl buffered_write\n"
    "cbnz w0, .Llog_arm64_done\n   adrp x1, log_writer_failed\n"
    "add x1, x1, :lo12:log_writer_failed\n   mov w2, #1\n   strb w2, [x1]\n"
    ".Llog_arm64_done:\n   ldr x30, [sp], #16\n" ASM_RET
    ASM_END(log)
);
#elif RISCV64
__asm__(
    ASM_SECTION
    ASM_FUNC(buffered_flush)
    "ld t0, 0(a2)\n   beqz t0, .Lbuffered_flush_riscv64_empty\n"
    "addi sp, sp, -32\n   sd a2, 0(sp)\n   sd t0, 8(sp)\n   sd ra, 24(sp)\n"
    "mv a2, t0\n   call system_write_all\n   ld t0, 0(sp)\n   ld t1, 8(sp)\n"
    "xor a0, a0, t1\n   seqz a0, a0\n   sd zero, 0(t0)\n   ld ra, 24(sp)\n"
    "addi sp, sp, 32\n"
    ASM_RET
    ".Lbuffered_flush_riscv64_empty:\n   li a0, 1\n" ASM_RET
    ASM_END(buffered_flush)

    ASM_FUNC(buffered_write)
    "beqz a5, .Lbuffered_write_riscv64_empty\n"
    "li t6, 0\n   tail buffered_write_core\n"
    ".Lbuffered_write_riscv64_empty:\n   li a0, 1\n" ASM_RET
    ASM_END(buffered_write)
    ASM_FUNC(buffered_write_deferred_equal)
    "beqz a5, .Lbuffered_write_deferred_riscv64_empty\n"
    "li t6, 1\n   tail buffered_write_core\n"
    ".Lbuffered_write_deferred_riscv64_empty:\n   li a0, 1\n" ASM_RET
    ASM_END(buffered_write_deferred_equal)
    ASM_LOCAL_FUNC(buffered_write_core)
    "bnez t6, .Lbuffered_write_riscv64_deferred\n"
    "bgeu a5, a2, .Lbuffered_write_riscv64_direct_entry\n"
    "j .Lbuffered_write_riscv64_buffer_entry\n"
    ".Lbuffered_write_riscv64_deferred:\n"
    "bltu a2, a5, .Lbuffered_write_riscv64_direct_entry\n"
    ".Lbuffered_write_riscv64_buffer_entry:\n"
    "sub t0, a2, a5\n   ld t1, 0(a3)\n"
    "bltu t0, t1, .Lbuffered_write_riscv64_flush_first\n"
    "addi sp, sp, -32\n   sd a3, 0(sp)\n   sd a5, 8(sp)\n"
    "sd ra, 24(sp)\n   add a0, a1, t1\n   mv a1, a4\n"
    "mv a2, a5\n   call memory_copy\n   ld a3, 0(sp)\n"
    "ld a5, 8(sp)\n   ld t0, 0(a3)\n   add t0, t0, a5\n"
    "sd t0, 0(a3)\n   li a0, 1\n   ld ra, 24(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    ".Lbuffered_write_riscv64_flush_first:\n"
    "addi sp, sp, -48\n   sd a1, 0(sp)\n   sd a3, 8(sp)\n"
    "sd a4, 16(sp)\n   sd a5, 24(sp)\n   sd ra, 40(sp)\n"
    "mv a2, a3\n   call buffered_flush\n"
    "beqz a0, .Lbuffered_write_riscv64_leave_48\n"
    "ld a0, 0(sp)\n   ld a1, 16(sp)\n   ld a2, 24(sp)\n   call memory_copy\n"
    "ld a3, 8(sp)\n   ld a5, 24(sp)\n   ld t0, 0(a3)\n"
    "add t0, t0, a5\n   sd t0, 0(a3)\n   li a0, 1\n"
    ".Lbuffered_write_riscv64_leave_48:\n   ld ra, 40(sp)\n   addi sp, sp, 48\n"
    ASM_RET
    ".Lbuffered_write_riscv64_direct_entry:\n"
    "addi sp, sp, -32\n   sd a0, 0(sp)\n   sd a4, 8(sp)\n"
    "sd a5, 16(sp)\n   sd ra, 24(sp)\n   mv a2, a3\n"
    "call buffered_flush\n   beqz a0, .Lbuffered_write_riscv64_direct_failed\n"
    "ld a0, 0(sp)\n   ld a1, 8(sp)\n   ld a2, 16(sp)\n   call system_write_all\n"
    "ld t0, 16(sp)\n   xor a0, a0, t0\n   seqz a0, a0\n"
    ".Lbuffered_write_riscv64_direct_failed:\n   ld ra, 24(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_LOCAL_END(buffered_write_core)

    ASM_FUNC(buffered_write_byte)
    "beqz a2, .Lbuffered_byte_riscv64_direct\n   ld t0, 0(a3)\n"
    "bgeu t0, a2, .Lbuffered_byte_riscv64_flush\n"
    "add t1, a1, t0\n   sb a4, 0(t1)\n   addi t0, t0, 1\n   sd t0, 0(a3)\n"
    ".Lbuffered_byte_riscv64_success:\n   li a0, 1\n"
    ASM_RET
    ".Lbuffered_byte_riscv64_direct:\n"
    "addi sp, sp, -16\n   sd ra, 8(sp)\n   sb a4, 0(sp)\n"
    "mv a1, sp\n   li a2, 1\n   call system_write_all\n"
    "addi a0, a0, -1\n   seqz a0, a0\n   ld ra, 8(sp)\n   addi sp, sp, 16\n"
    ASM_RET
    ".Lbuffered_byte_riscv64_flush:\n"
    "addi sp, sp, -32\n   sd a1, 0(sp)\n   sd a3, 8(sp)\n"
    "sd a4, 16(sp)\n   sd ra, 24(sp)\n   mv a2, a3\n"
    "call buffered_flush\n   ld a1, 0(sp)\n   ld a3, 8(sp)\n"
    "beqz a0, .Lbuffered_byte_riscv64_failed\n"
    "ld a4, 16(sp)\n   sb a4, 0(a1)\n   li t0, 1\n"
    "sd t0, 0(a3)\n   li a0, 1\n"
    ".Lbuffered_byte_riscv64_failed:\n   ld ra, 24(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_END(buffered_write_byte)

    ASM_FUNC(log_direct)
    "addi sp, sp, -16\n   sd a1, 0(sp)\n   sd ra, 8(sp)\n"
    "mv a2, a1\n   mv a1, a0\n   li a0, 1\n"
    "call system_write_all\n   ld t0, 0(sp)\n   beq a0, t0, .Llog_direct_riscv64_done\n"
    "lla t1, log_writer_failed\n   li t0, 1\n   sb t0, 0(t1)\n"
    ".Llog_direct_riscv64_done:\n   ld ra, 8(sp)\n   addi sp, sp, 16\n"
    ASM_RET
    ASM_END(log_direct)

    ASM_FUNC(log_flush)
    "addi sp, sp, -16\n   sd ra, 8(sp)\n"
    "li a0, 1\n   lla a1, log_writer_buffer\n"
    "lla a2, log_writer_buffer_length\n   call buffered_flush\n"
    "bnez a0, .Llog_flush_riscv64_done\n   lla t0, log_writer_failed\n"
    "li t1, 1\n   sb t1, 0(t0)\n"
    ".Llog_flush_riscv64_done:\n   ld ra, 8(sp)\n   addi sp, sp, 16\n" ASM_RET
    ASM_END(log_flush)

    ASM_FUNC(log_failure_reset)
    "lla a0, log_writer_failed\n   sb zero, 0(a0)\n" ASM_RET
    ASM_END(log_failure_reset)

    ASM_FUNC(log_failed)
    "lla a0, log_writer_failed\n   lbu a0, 0(a0)\n" ASM_RET
    ASM_END(log_failed)

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
    "bnez a1, .Llog_riscv64_have_length\n"
    "addi sp, sp, -16\n   sd s0, 0(sp)\n   sd ra, 8(sp)\n"
    "mv s0, a0\n   call string_length\n   mv a1, a0\n   mv a0, s0\n"
    "ld s0, 0(sp)\n   ld ra, 8(sp)\n   addi sp, sp, 16\n"
    ".Llog_riscv64_have_length:\n   mv a5, a1\n   mv a4, a0\n"
    "lla a3, log_writer_buffer_length\n   li a2, 4096\n"
    "lla a1, log_writer_buffer\n   li a0, 1\n"
    "addi sp, sp, -16\n   sd ra, 8(sp)\n   call buffered_write\n"
    "bnez a0, .Llog_riscv64_done\n   lla t0, log_writer_failed\n"
    "li t1, 1\n   sb t1, 0(t0)\n"
    ".Llog_riscv64_done:\n   ld ra, 8(sp)\n   addi sp, sp, 16\n" ASM_RET
    ASM_END(log)
);
#else
#error "no logging implementation for this architecture"
#endif

#undef RUNTIME_TEXT
#undef RUNTIME_TEXT_INNER
