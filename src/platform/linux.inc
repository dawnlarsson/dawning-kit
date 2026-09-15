/*
        Experimental C standard library

        Linux platform specific functions and definitions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_LINUX
#define STANDARD_MODERN_C_PLATFORM_LINUX

#include "spark.inc"

struct linux_dirent64
{
        p64 d_ino;
        p64 d_off;
        p16 d_reclen;
        p8 d_type;
        p8 d_name[];
};

positive2 term_size();
typedef struct { positive bytes; bipolar error; } system_write_result;
positive system_write_all(positive handle, const address_any data, positive length);
system_write_result system_write_all_checked(positive handle, const address_any data,
                                             positive length);
bipolar system_read_retry(positive handle, address_any data, positive length);
bipolar system_wait4_retry(bipolar process, address_any status,
                           positive options, address_any usage);
bipolar file_slurp(string_address path, p8 address_to into, positive capacity);
positive program_initial_identity(void);

/*
        THE THREAD BLOCK

        Every thread of a program built on this library, the first one
        included, has one of these, and the architecture's thread register
        points at it: fs on x86_64, tpidr_el0 on arm64, tp on riscv64. The
        first thread's is thread_main in .bss, installed by _start before
        anything else runs; thread_start maps one at the top of every new
        thread's stack and hands it to clone as the TLS word.

        Why an explicit block and not __thread. The two compile to the same
        instruction -- a load at a fixed offset from the thread register -- so
        access cost does not choose between them. Everything around the load
        does. A __thread object needs .tdata and .tbss rules in spark.ld, an
        initial image copied into every new thread, and three conventions for
        where the register points relative to the data: x86_64 keeps the
        data below fs, arm64 sixteen bytes above tpidr_el0, riscv64 at tp.
        Hand-written assembly would reach it through a different
        linker-computed relocation on each machine. This block sits at
        positive offsets from the register on all three, written down once
        below, reachable from assembly as a constant and from C through
        __builtin_thread_pointer(), and a new thread's copy is a zeroed
        mapping with four words stored into it. spark.ld discards .tdata and
        .tbss, so a __thread object -- which would read some other field of
        this block -- fails at the link instead of at run time.

        What a program that never starts a thread pays for it: one
        arch_prctl(ARCH_SET_FS) trap in x86_64 _start and one register write
        on the other two. errno and the allocator's shelves live here, and a
        load through fs or tp is the same single instruction the plain .bss
        objects were.

        self points at the block because x86_64 GCC spells
        __builtin_thread_pointer() as a load of %fs:0. identity is the thread
        id word that clone's PARENT_SETTID fills and CHILD_CLEARTID zeroes
        when the thread has gone; thread_join sleeps on it. region is the
        base of the thread's mapping, zero for the first thread. run is the
        parallel run the thread is working inside, if any, which is how a
        nested call knows to run inline. slot is the thread's pool slot, zero
        for the first thread. shelves are the allocator's free-list heads,
        one per size class: memory_take and memory_give reach them at
        THREAD_OFFSET_SHELVES, so a thread allocates and frees without a lock.
*/
#define THREAD_OFFSET_ERROR 8
#define THREAD_OFFSET_IDENTITY 12
#define THREAD_OFFSET_REGION 16
#define THREAD_OFFSET_RUN 24
#define THREAD_OFFSET_ENTRY 32
#define THREAD_OFFSET_ARGUMENT 40
#define THREAD_OFFSET_SLOT 48
#define THREAD_OFFSET_SHELVES 64
#define THREAD_SHELVES 52
#define THREAD_BLOCK_BYTES 512

/* A thread's whole mapping: a guard page at the bottom, the stack above it,
   and the block at the top where the stack starts. Eight megabytes of
   address, the stack a Linux main thread gets by default, committed only as
   it is touched. */
#define THREAD_MAPPING_BYTES 8388608
#define THREAD_GUARD_BYTES 4096

typedef struct thread
{
        struct thread address_to self;
        b32 error_number;
        b32 identity;
        address_any region;
        address_any run;
        fn(address_to entry)(address_any);
        address_any argument;
        positive slot;
        positive spare;
        address_any shelves[THREAD_SHELVES];
        positive reserved[4];
} thread;

_Static_assert(__builtin_offsetof(thread, error_number) == THREAD_OFFSET_ERROR &&
               __builtin_offsetof(thread, identity) == THREAD_OFFSET_IDENTITY &&
               __builtin_offsetof(thread, region) == THREAD_OFFSET_REGION &&
               __builtin_offsetof(thread, run) == THREAD_OFFSET_RUN &&
               __builtin_offsetof(thread, entry) == THREAD_OFFSET_ENTRY &&
               __builtin_offsetof(thread, argument) == THREAD_OFFSET_ARGUMENT &&
               __builtin_offsetof(thread, slot) == THREAD_OFFSET_SLOT &&
               __builtin_offsetof(thread, shelves) == THREAD_OFFSET_SHELVES &&
               sizeof(thread) == THREAD_BLOCK_BYTES,
               "the thread block offsets the assembly spells out");

//      The first thread's block, and the number of threads thread_start has
//      made that thread_join has not yet reaped. Zero is one thread.
extern thread thread_main;
extern positive threads_live;

#define thread_self() ((thread address_to)__builtin_thread_pointer())

/*
        A lock is one 32 bit word: 0 free, 1 held, 2 held with somebody
        possibly asleep on it. Zero is free, so a lock in .bss needs no
        initialiser. The shared spellings for a lock two processes hold over a
        MAP_SHARED page are in standard.c.
*/
#define LIBRARY_THREAD_RUNTIME 1
#define LOCK_FREE 0
#define LOCK_HELD 1
#define LOCK_WAITED 2

typedef struct
{
        b32 word;
} lock;

fn lock_take(lock address_to it);
fn lock_release(lock address_to it);
bool lock_try(lock address_to it);
bipolar thread_wait(b32 address_to word, b32 expected);
bipolar thread_wake(b32 address_to word, b32 count);

#define atomic_load(address) __atomic_load_n(address, __ATOMIC_SEQ_CST)

/*
        WHAT A SECOND THREAD MAY TOUCH

        The shared-state audit of library.c, library.common.c and standard.c
        made when threads arrived (2026-09-14). Utilities never start threads
        themselves; they hand jobs to the pool, and a job runs on a worker.

        Safe from any thread:
          - every pure library.c leaf: memory_*, string_*, the hashes, the
            codec and crypto cores whose state lives in a caller's struct;
          - memory and memory_free (the mmap and munmap traps),
            memory_growth, memory_reserve and memory_release on storage one
            thread owns at a time;
          - malloc, free, calloc, realloc and the aligned forms: per-thread
            shelves, with the shared chunk and depot behind allocator_lock;
          - errno, which is the calling thread's;
          - lock_take, lock_release, lock_try, thread_wait, thread_wake,
            the atomic_* builtins and raw system calls;
          - memory_arena_take and memory_vector_grow over an arena the job
            owns;
          - reading cpu_has_*, program_stack_base, program_entry_identity and
            program_words, which _start and main write once before any
            thread exists.

        First thread only, by the same rule glibc gives its non-reentrant
        interfaces -- one shared cell each, and no lock:
          - log, log_error, log_flush, string_format(log, ...) and the
            diagnostics built on them (log_writer_buffer in library.c);
          - buffered_write, buffered_reserve and buffered_flush on a buffer
            more than one thread can reach (a job's own buffer is fine);
          - working_directory and its setters, and text_token_place, the
            strtok position;
          - in standard.c: the environment (getenv, setenv, putenv,
            environ), atexit/at_quick_exit and exit, rand/random state, every
            FILE stream (no per-stream lock), the static buffers behind
            strerror's unknown text, asctime/ctime/gmtime/localtime, tmpnam,
            basename/dirname, getopt, and the popen/pclose spool table;
          - src/sh/file.c's utility_arena and its take/grow/read_all wrappers:
            the mapping is safe, the bump state is one static.

        A worker's output reaches a descriptor only through the pool's
        ordered writer, whose sink runs on the first thread.
*/

#define LINUX_RUNTIME_TEXT_INNER(value) #value
#define LINUX_RUNTIME_TEXT(value) LINUX_RUNTIME_TEXT_INNER(value)

/*
        Linux enters _start with the original process stack, not through the C
        ABI. _start records that exact address before aligning the call stack,
        detects the CPU before any dispatched primitive runs, and passes
        main's signed 32-bit result to exit. There is no C-side startup shim
        and no extra call between the kernel entry and that sequence.

        term_size returns the two-word positive2 in the architecture's normal
        aggregate return registers. Its eight-byte winsize scratch stays on
        the aligned stack, and only a zero ioctl result replaces the 80 by 24
        fallback. system_write_all keeps issuing write(2) from the first byte
        not yet accepted while the syscall makes positive progress, returns
        the number accepted, and deliberately does not retry zero, EINTR, or
        any other negative result. A zero length makes no syscall. That is the
        common contract of the shell's former write loops, not a stronger I/O
        policy hidden behind their name.

        system_write_all_checked aliases that same loop and also returns the
        last nonpositive syscall result: zero on completion, zero length, or
        zero progress; negative errno on failure, even after a written prefix.
        The byte count distinguishes completion from a zero-progress stop.

        system_read_retry is the complementary single-read primitive: it
        retries exactly EINTR, including for a zero length, and returns every
        other kernel result unchanged. Its implementation deliberately leaves
        the data and length argument registers, and the spare caller-saved
        registers used below, untouched. file_slurp keeps its loop state there
        across the direct call: arm64 and riscv64 stay stackless, while x86_64
        reserves just the one word its call-site alignment requires.

        system_wait4_retry likewise retries only EINTR and otherwise exposes
        wait4's result unchanged. x86_64 moves the ABI's fourth argument from
        rcx to the syscall ABI's r10 before trapping. arm64 and riscv64 keep
        status, options and rusage in their ABI registers and reload the pid
        that each trap replaces with its result.

        file_slurp reserves one byte for a terminator and fills the rest through
        EOF or capacity. Zero capacity is a query that touches neither pointer
        and makes no syscall; capacity one still validates the path, writes the
        terminator and closes it. openat and read retry EINTR. An open error
        leaves the destination untouched; another read error terminates what
        was accepted, closes once and returns that error. The private descriptor
        is opened close-on-exec so a concurrent fork/exec cannot leak it into
        the new image. close itself is never retried: after EINTR Linux no
        longer gives userspace a portable answer about descriptor ownership.

        sleep and exit trap directly rather than paying for a generic
        syscall-arity adapter.
*/
#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "xor %r8d, %r8d\n   test %rdx, %rdx\n"
    "jz .Lsystem_write_all_x64_done\n"
    ".Lsystem_write_all_x64_more:\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(write)) ", %eax\n   syscall\n"
    "test %rax, %rax\n   jle .Lsystem_write_all_x64_failed\n"
    "add %rax, %r8\n   add %rax, %rsi\n   sub %rax, %rdx\n"
    "jnz .Lsystem_write_all_x64_more\n"
    ".Lsystem_write_all_x64_done:\n"
    "mov %r8, %rax\n"
    ASM_RET
    ".Lsystem_write_all_x64_failed:\n"
    "mov %rax, %rdx\n   mov %r8, %rax\n"
    ASM_RET
    ASM_END(system_write_all)
    ASM_ALIAS(system_write_all_checked, system_write_all)

    ASM_FUNC(system_read_retry)
    ".Lsystem_read_retry_x64_more:\n"
    "xor %eax, %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lsystem_read_retry_x64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mov %rcx, %r10\n"
    ".Lsystem_wait4_retry_x64_more:\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(wait4)) ", %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lsystem_wait4_retry_x64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "test %rdx, %rdx\n   jz .Lfile_slurp_x64_zero\n"
    "mov %rsi, %r8\n   mov %rdx, %r9\n   mov %rdi, %rsi\n"
    ".Lfile_slurp_x64_open:\n"
    "mov $-100, %rdi\n   mov $0x80000, %edx\n   xor %r10d, %r10d\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(openat)) ", %eax\n   syscall\n"
    "cmp $-4, %rax\n   je .Lfile_slurp_x64_open\n"
    "test %rax, %rax\n   js .Lfile_slurp_x64_return\n"
    "mov %rax, %rdi\n   mov %r8, %rsi\n   dec %r9\n"
    "jnz .Lfile_slurp_x64_save\n"
    "movb $0, (%rsi)\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(close)) ", %eax\n   syscall\n"
    "xor %eax, %eax\n"
    ASM_RET
    ".Lfile_slurp_x64_save:\n"
    "mov %r9, %rdx\n   xor %r8d, %r8d\n   sub $8, %rsp\n"
    ".Lfile_slurp_x64_read:\n"
    "call system_read_retry\n"
    "test %rax, %rax\n   jz .Lfile_slurp_x64_terminate\n"
    "js .Lfile_slurp_x64_read_error\n"
    "add %rax, %rsi\n   add %rax, %r8\n   sub %rax, %rdx\n"
    "jnz .Lfile_slurp_x64_read\n"
    "jmp .Lfile_slurp_x64_terminate\n"
    ".Lfile_slurp_x64_read_error:\n"
    "mov %rax, %r8\n"
    ".Lfile_slurp_x64_terminate:\n"
    "movb $0, (%rsi)\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(close)) ", %eax\n   syscall\n"
    "mov %r8, %rax\n   add $8, %rsp\n"
    ASM_RET
    ".Lfile_slurp_x64_zero:\n"
    "xor %eax, %eax\n"
    ".Lfile_slurp_x64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "sub $16, %rsp\n   mov $1, %edi\n   mov $0x5413, %esi\n"
    "mov %rsp, %rdx\n   mov $" LINUX_RUNTIME_TEXT(syscall(ioctl)) ", %eax\n"
    "syscall\n   test %rax, %rax\n   jnz .Lterm_size_x64_default\n"
    "movzwq 2(%rsp), %rax\n   movzwq (%rsp), %rdx\n"
    "jmp .Lterm_size_x64_done\n"
    ".Lterm_size_x64_default:\n"
    "mov $80, %eax\n   mov $24, %edx\n"
    ".Lterm_size_x64_done:\n"
    "add $16, %rsp\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "xor %esi, %esi\n   xor %edx, %edx\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(nanosleep)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "movslq %edi, %rdi\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(exit_group)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(program_initial_identity)
    "mov program_entry_identity(%rip), %rax\n   test %rax, %rax\n"
    "jnz .Lprogram_initial_identity_x64_done\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(getpid)) ", %eax\n   syscall\n"
    ".Lprogram_initial_identity_x64_done:\n"
    ASM_RET
    ASM_END(program_initial_identity)

    ASM_FUNC(_start)
    "xor %ebp, %ebp\n   mov %rsp, program_stack_base(%rip)\n"
    "and $-16, %rsp\n"
    /* The first thread's block goes in before anything can read errno or
       pop an allocator shelf. r12-r14 still carry the Spark handoff, and the
       trap spends only rax, rcx and r11. */
    "lea thread_main(%rip), %rsi\n   mov %rsi, (%rsi)\n"
    "mov $0x1002, %edi\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(arch_prctl)) ", %eax\n   syscall\n"
    /* A Spark loader supplies the already enabled CPU feature set.  Keep the
       complete detector as the stock-kernel/ELF fallback rather than making
       the runtime depend on Moonwater merely to start. */
    "movabs $" LINUX_RUNTIME_TEXT(SPARK_START_MAGIC) ", %rax\n"
    "cmp %rax, %r12\n   jne .Lstart_x64_detect\n"
    "mov %r14, program_entry_identity(%rip)\n"
    /* The private feature word carries one already-normalised Boolean byte
       for each public runtime flag.  Publishing those bytes directly is the
       hardware floor: byte stores and shifts, with no policy
       branch left for userspace to rediscover. */
    "mov %r13b, cpu_has_avx2(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_avx512(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_avx512_vbmi(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_fma(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_pclmul(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_aes(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_vpclmul(%rip)\n   shr $8, %r13\n"
    "mov %r13b, cpu_has_vaes(%rip)\n"
    ".Lstart_x64_ready:\n   call main\n"
    "mov %eax, %edi\n   call exit\n   hlt\n"
    /* Put the compatibility detector behind the non-returning exit path.
       Spark now falls straight into main; an ordinary ELF pays one jump after
       the much more expensive CPUID/XGETBV fallback. */
    ".Lstart_x64_detect:\n   call moonwater_cpu_detect\n"
    "jmp .Lstart_x64_ready\n"
    ASM_END(_start)
);
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "mov x3, x0\n   mov x4, xzr\n   cbz x2, .Lsystem_write_all_arm64_done\n"
    ".Lsystem_write_all_arm64_more:\n"
    "mov x0, x3\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(write)) "\n"
    SYSCALL_INSTRUCTION "\n   cmp x0, #0\n"
    "b.le .Lsystem_write_all_arm64_failed\n"
    "add x4, x4, x0\n   add x1, x1, x0\n   sub x2, x2, x0\n"
    "cbnz x2, .Lsystem_write_all_arm64_more\n"
    ".Lsystem_write_all_arm64_done:\n"
    "mov x1, xzr\n   mov x0, x4\n"
    ASM_RET
    ".Lsystem_write_all_arm64_failed:\n"
    "mov x1, x0\n   mov x0, x4\n"
    ASM_RET
    ASM_END(system_write_all)
    ASM_ALIAS(system_write_all_checked, system_write_all)

    ASM_FUNC(system_read_retry)
    "mov x3, x0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(read)) "\n"
    ".Lsystem_read_retry_arm64_more:\n"
    "mov x0, x3\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n"
    "b.eq .Lsystem_read_retry_arm64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mov x4, x0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(wait4)) "\n"
    ".Lsystem_wait4_retry_arm64_more:\n"
    "mov x0, x4\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n"
    "b.eq .Lsystem_wait4_retry_arm64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "cbz x2, .Lfile_slurp_arm64_zero\n"
    "mov x4, x1\n   mov x5, x2\n   mov x6, x0\n"
    ".Lfile_slurp_arm64_open:\n"
    "mov x0, #-100\n   mov x1, x6\n   mov x2, #0x80000\n   mov x3, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(openat)) "\n"
    SYSCALL_INSTRUCTION "\n   cmn x0, #4\n   b.eq .Lfile_slurp_arm64_open\n"
    "tbnz x0, #63, .Lfile_slurp_arm64_return\n"
    "mov x3, x0\n   sub x5, x5, #1\n"
    "cbnz x5, .Lfile_slurp_arm64_save\n"
    "strb wzr, [x4]\n   mov x0, x3\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(close)) "\n"
    SYSCALL_INSTRUCTION "\n   mov x0, xzr\n"
    ASM_RET
    ".Lfile_slurp_arm64_save:\n"
    "mov x1, x4\n   mov x2, x5\n   mov x4, x3\n   mov x5, xzr\n"
    "mov x6, x30\n"
    ".Lfile_slurp_arm64_read:\n"
    "mov x0, x4\n"
    "bl system_read_retry\n"
    "cbz x0, .Lfile_slurp_arm64_terminate\n"
    "tbnz x0, #63, .Lfile_slurp_arm64_read_error\n"
    "add x1, x1, x0\n   add x5, x5, x0\n   sub x2, x2, x0\n"
    "cbnz x2, .Lfile_slurp_arm64_read\n"
    "b .Lfile_slurp_arm64_terminate\n"
    ".Lfile_slurp_arm64_read_error:\n"
    "mov x5, x0\n"
    ".Lfile_slurp_arm64_terminate:\n"
    "strb wzr, [x1]\n   mov x0, x4\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(close)) "\n"
    SYSCALL_INSTRUCTION "\n   mov x0, x5\n   ret x6\n"
    ".Lfile_slurp_arm64_zero:\n"
    "mov x0, xzr\n"
    ".Lfile_slurp_arm64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "sub sp, sp, #16\n   mov x0, #1\n   mov x1, #0x5413\n"
    "mov x2, sp\n   mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(ioctl)) "\n"
    SYSCALL_INSTRUCTION "\n   cbnz x0, .Lterm_size_arm64_default\n"
    "ldrh w0, [sp, #2]\n   ldrh w1, [sp]\n"
    "b .Lterm_size_arm64_done\n"
    ".Lterm_size_arm64_default:\n"
    "mov x0, #80\n   mov x1, #24\n"
    ".Lterm_size_arm64_done:\n"
    "add sp, sp, #16\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "mov x1, xzr\n   mov x2, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(nanosleep)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "sxtw x0, w0\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(exit_group)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(program_initial_identity)
    "adrp x1, program_entry_identity\n"
    "ldr x0, [x1, :lo12:program_entry_identity]\n   cbnz x0, 1f\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(getpid)) "\n"
    SYSCALL_INSTRUCTION "\n"
    "1:\n"
    ASM_RET
    ASM_END(program_initial_identity)

    ASM_FUNC(_start)
    "mov x29, #0\n   mov x30, #0\n   mov x0, sp\n"
    /* The first thread's block: tpidr_el0 is a register EL0 may write. */
    "adrp x1, thread_main\n   add x1, x1, :lo12:thread_main\n"
    "str x1, [x1]\n   msr tpidr_el0, x1\n"
    "adrp x1, program_stack_base\n   str x0, [x1, :lo12:program_stack_base]\n"
    "movz x1, #0x4152\n   movk x1, #0x5354, lsl #16\n"
    "movk x1, #0x524b, lsl #32\n   movk x1, #0x5350, lsl #48\n"
    "cmp x19, x1\n   b.ne .Lstart_arm64_detect\n"
    "adrp x1, program_entry_identity\n"
    "str x21, [x1, :lo12:program_entry_identity]\n"
    /* The Spark feature word: the carry-less multiply in the low byte, AES
       in the next, published as they come. */
    "adrp x1, cpu_has_pclmul\n   strb w20, [x1, :lo12:cpu_has_pclmul]\n"
    "lsr x20, x20, #8\n"
    "adrp x1, cpu_has_aes\n   strb w20, [x1, :lo12:cpu_has_aes]\n"
    ".Lstart_arm64_ready:\n"
    "and sp, x0, #-16\n   bl main\n"
    "bl exit\n   brk #0\n"
    /* An ordinary ELF asks the auxiliary vector, from the stack base kept
       above; the call spends x0, which is reloaded. */
    ".Lstart_arm64_detect:\n   bl moonwater_cpu_detect\n"
    "adrp x1, program_stack_base\n   ldr x0, [x1, :lo12:program_stack_base]\n"
    "b .Lstart_arm64_ready\n"
    ASM_END(_start)
);
#elif RISCV64
__asm__(
    ASM_SECTION
    ASM_FUNC(system_write_all)
    "mv t0, a0\n   li t1, 0\n   beqz a2, .Lsystem_write_all_riscv64_done\n"
    ".Lsystem_write_all_riscv64_more:\n"
    "mv a0, t0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(write)) "\n   ecall\n"
    "blez a0, .Lsystem_write_all_riscv64_failed\n"
    "add t1, t1, a0\n   add a1, a1, a0\n   sub a2, a2, a0\n"
    "bnez a2, .Lsystem_write_all_riscv64_more\n"
    ".Lsystem_write_all_riscv64_done:\n"
    "li a1, 0\n   mv a0, t1\n"
    ASM_RET
    ".Lsystem_write_all_riscv64_failed:\n"
    "mv a1, a0\n   mv a0, t1\n"
    ASM_RET
    ASM_END(system_write_all)
    ASM_ALIAS(system_write_all_checked, system_write_all)

    ASM_FUNC(system_read_retry)
    "mv t1, a0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(read)) "\n"
    "li t0, -4\n"
    ".Lsystem_read_retry_riscv64_more:\n"
    "mv a0, t1\n   ecall\n"
    "beq a0, t0, .Lsystem_read_retry_riscv64_more\n"
    ASM_RET
    ASM_END(system_read_retry)

    ASM_FUNC(system_wait4_retry)
    "mv t0, a0\n   li a7, " LINUX_RUNTIME_TEXT(syscall(wait4)) "\n"
    "li t1, -4\n"
    ".Lsystem_wait4_retry_riscv64_more:\n"
    "mv a0, t0\n   ecall\n"
    "beq a0, t1, .Lsystem_wait4_retry_riscv64_more\n"
    ASM_RET
    ASM_END(system_wait4_retry)

    ASM_FUNC(file_slurp)
    "beqz a2, .Lfile_slurp_riscv64_zero\n"
    "mv t0, a0\n   mv t1, a1\n   mv t2, a2\n"
    ".Lfile_slurp_riscv64_open:\n"
    "li a0, -100\n   mv a1, t0\n   li a2, 0x80000\n   li a3, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(openat)) "\n   ecall\n"
    "li t3, -4\n   beq a0, t3, .Lfile_slurp_riscv64_open\n"
    "bltz a0, .Lfile_slurp_riscv64_return\n"
    "mv t3, a0\n   addi t2, t2, -1\n"
    "bnez t2, .Lfile_slurp_riscv64_save\n"
    "sb zero, 0(t1)\n   mv a0, t3\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(close)) "\n   ecall\n"
    "li a0, 0\n"
    ASM_RET
    ".Lfile_slurp_riscv64_save:\n"
    "mv a1, t1\n   mv a2, t2\n   mv t2, t3\n   li t3, 0\n"
    "mv t4, ra\n"
    ".Lfile_slurp_riscv64_read:\n"
    "mv a0, t2\n"
    "call system_read_retry\n"
    "beqz a0, .Lfile_slurp_riscv64_terminate\n"
    "bltz a0, .Lfile_slurp_riscv64_read_error\n"
    "add a1, a1, a0\n   add t3, t3, a0\n   sub a2, a2, a0\n"
    "bnez a2, .Lfile_slurp_riscv64_read\n"
    "j .Lfile_slurp_riscv64_terminate\n"
    ".Lfile_slurp_riscv64_read_error:\n"
    "mv t3, a0\n"
    ".Lfile_slurp_riscv64_terminate:\n"
    "sb zero, 0(a1)\n   mv a0, t2\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(close)) "\n   ecall\n"
    "mv a0, t3\n   mv ra, t4\n"
    ASM_RET
    ".Lfile_slurp_riscv64_zero:\n"
    "li a0, 0\n"
    ".Lfile_slurp_riscv64_return:\n"
    ASM_RET
    ASM_END(file_slurp)

    ASM_FUNC(term_size)
    "addi sp, sp, -16\n   li a0, 1\n   li a1, 0x5413\n"
    "mv a2, sp\n   li a7, " LINUX_RUNTIME_TEXT(syscall(ioctl)) "\n   ecall\n"
    "bnez a0, .Lterm_size_riscv64_default\n"
    "lhu a0, 2(sp)\n   lhu a1, 0(sp)\n"
    "j .Lterm_size_riscv64_done\n"
    ".Lterm_size_riscv64_default:\n"
    "li a0, 80\n   li a1, 24\n"
    ".Lterm_size_riscv64_done:\n"
    "addi sp, sp, 16\n"
    ASM_RET
    ASM_END(term_size)

    ASM_FUNC(sleep)
    "li a1, 0\n   li a2, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(nanosleep)) "\n   ecall\n"
    ASM_RET
    ASM_END(sleep)

    ASM_FUNC(exit)
    "sext.w a0, a0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(exit_group)) "\n   ecall\n"
    ASM_RET
    ASM_END(exit)

    ASM_FUNC(program_initial_identity)
    "lla t0, program_entry_identity\n   ld a0, 0(t0)\n"
    "bnez a0, 1f\n   li a7, " LINUX_RUNTIME_TEXT(syscall(getpid)) "\n   ecall\n"
    "1:\n"
    ASM_RET
    ASM_END(program_initial_identity)

    ASM_FUNC(_start)
    /* The RISC-V psABI reserves gp for __global_pointer$.  GNU ld relaxes
       accesses to nearby small globals into gp-relative loads and stores,
       including C generated after this entry point.  A kernel enters with
       gp unspecified, so establish it before the first call.  norelax is
       essential here: relaxing the initializer itself into mv gp,gp would
       preserve the unknown incoming value. */
    ".option push\n   .option norelax\n"
    "lla gp, __global_pointer$\n"
    ".option pop\n"
    "li s0, 0\n   li ra, 0\n   mv a0, sp\n"
    /* The first thread's block: tp is an ordinary register here. */
    "lla tp, thread_main\n   sd tp, 0(tp)\n"
    "lla t0, program_stack_base\n   sd a0, 0(t0)\n"
    "li t1, " LINUX_RUNTIME_TEXT(SPARK_START_MAGIC) "\n"
    "bne s2, t1, .Lstart_riscv64_detect\n"
    "lla t0, program_entry_identity\n   sd s4, 0(t0)\n"
    /* The Spark feature word: Zbc in the low byte, Zvkned with usable V in
       the next, published as they come. */
    "lla t0, cpu_has_pclmul\n   sb s3, 0(t0)\n   srli s3, s3, 8\n"
    "lla t0, cpu_has_aes\n   sb s3, 0(t0)\n"
    ".Lstart_riscv64_ready:\n"
    "andi sp, sp, -16\n   call main\n"
    "call exit\n   ebreak\n"
    /* An ordinary ELF asks riscv_hwprobe. */
    ".Lstart_riscv64_detect:\n   call moonwater_cpu_detect\n"
    "j .Lstart_riscv64_ready\n"
    ASM_END(_start)
);
#else
#error "no _start defined for this architecture"
#endif

/*
        The lock, the futex word, and the two objects every thread reaches.

        threads_live counts the threads thread_start has made and thread_join
        has not yet reaped, and zero -- a process of one thread -- is the
        whole of the lock elision. While it reads zero a take is a load, a
        branch and a plain store of 1, and a release the same with 0: no
        bus-locked instruction and no exclusive pair. The store is kept, not
        skipped, so that a thread started while the lock is held finds it
        held. The two paths mix safely in both directions. A word stored
        plainly reads 1 to the next compare-and-swap. A word a waiter left at
        2 is released by a plain store of 0 once that waiter has been joined,
        when there is nobody left to wake. thread_start increments the count
        before its clone, so no second thread runs while it reads zero, and
        only the last thread standing can see it return to zero.

        Taking, with threads about: one compare-and-swap of 0 to 1. Losing
        it, exchange 2 in and sleep on the word for as long as the exchange
        keeps answering non-zero. This is Drepper's three-state lock, which
        standard.c carried in C until it moved here; its note on why the slow
        path exchanges rather than tests is still beside the shared
        spellings. Releasing: exchange 0 in, and wake one sleeper only when
        the word said 2.

        The futex is the private one, keyed by address space, because every
        lock here belongs to threads of one process. A lock that lives in a
        MAP_SHARED page between processes must use lock_take_shared in
        standard.c, which never elides and keys its futex by page.

        thread_wait and thread_wake are the bare private futex word. wait
        sleeps only while the word still holds expected and may return early
        for any reason at all, so every caller reads the word again; wake
        answers how many sleepers it woke.

        Measured on the 9950X before this existed: an uncontended take and
        release through the C compare-and-swap pair cost 7.57 ns against
        1.70 ns for the malloc and free it would have wrapped.
*/
__asm__(
    ASM_BSS_OBJECT_BEGIN(thread_main, 64)
    ASM_ZERO(THREAD_BLOCK_BYTES)
    ASM_OBJECT_END(thread_main)
    ASM_BSS_OBJECT_BEGIN(threads_live, 8)
    ".zero 8\n"
    ASM_OBJECT_END(threads_live)
);

#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(lock_take)
    "cmpq $0, threads_live(%rip)\n   jne .Llock_take_x64_shared\n"
    "movl $1, (%rdi)\n"
    ASM_RET
    ".Llock_take_x64_shared:\n"
    "xor %eax, %eax\n   mov $1, %edx\n"
    "lock cmpxchg %edx, (%rdi)\n   jne .Llock_take_x64_contended\n"
    ASM_RET
    ".Llock_take_x64_contended:\n"
    "mov $2, %edx\n   xchg %edx, (%rdi)\n   test %edx, %edx\n"
    "jz .Llock_take_x64_done\n"
    "mov $128, %esi\n   mov $2, %edx\n   xor %r10d, %r10d\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(futex)) ", %eax\n   syscall\n"
    "jmp .Llock_take_x64_contended\n"
    ".Llock_take_x64_done:\n"
    ASM_RET
    ASM_END(lock_take)

    ASM_FUNC(lock_release)
    "cmpq $0, threads_live(%rip)\n   jne .Llock_release_x64_shared\n"
    "movl $0, (%rdi)\n"
    ASM_RET
    ".Llock_release_x64_shared:\n"
    "xor %eax, %eax\n   xchg %eax, (%rdi)\n   cmp $1, %eax\n"
    "jne .Llock_release_x64_wake\n"
    ASM_RET
    ".Llock_release_x64_wake:\n"
    "mov $129, %esi\n   mov $1, %edx\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(futex)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(lock_release)

    ASM_FUNC(lock_try)
    "cmpq $0, threads_live(%rip)\n   jne .Llock_try_x64_shared\n"
    "xor %eax, %eax\n   cmpl $0, (%rdi)\n   jne .Llock_try_x64_done\n"
    "movl $1, (%rdi)\n   mov $1, %eax\n"
    ".Llock_try_x64_done:\n"
    ASM_RET
    ".Llock_try_x64_shared:\n"
    "xor %eax, %eax\n   mov $1, %edx\n   lock cmpxchg %edx, (%rdi)\n"
    "sete %al\n   movzbl %al, %eax\n"
    ASM_RET
    ASM_END(lock_try)

    ASM_FUNC(thread_wait)
    "mov %esi, %edx\n   mov $128, %esi\n   xor %r10d, %r10d\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(futex)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(thread_wait)

    ASM_FUNC(thread_wake)
    "mov %esi, %edx\n   mov $129, %esi\n"
    "mov $" LINUX_RUNTIME_TEXT(syscall(futex)) ", %eax\n   syscall\n"
    ASM_RET
    ASM_END(thread_wake)
);
#elif ARM64
__asm__(
    ASM_SECTION
    //      Exclusive pairs rather than LSE's cas, which armv8.0 does not have.
    ASM_FUNC(lock_take)
    "adrp x1, threads_live\n   ldr x1, [x1, :lo12:threads_live]\n"
    "cbnz x1, .Llock_take_arm64_shared\n"
    "mov w1, #1\n   str w1, [x0]\n"
    ASM_RET
    ".Llock_take_arm64_shared:\n"
    "mov w1, #1\n"
    ".Llock_take_arm64_claim:\n"
    "ldxr w2, [x0]\n   cbnz w2, .Llock_take_arm64_lost\n"
    "stxr w3, w1, [x0]\n   cbnz w3, .Llock_take_arm64_claim\n"
    ASM_RET
    ".Llock_take_arm64_lost:\n"
    "clrex\n"
    ".Llock_take_arm64_contended:\n"
    "mov w1, #2\n"
    ".Llock_take_arm64_exchange:\n"
    "ldxr w2, [x0]\n   stxr w3, w1, [x0]\n"
    "cbnz w3, .Llock_take_arm64_exchange\n"
    "cbz w2, .Llock_take_arm64_done\n"
    "mov x4, x0\n   mov x1, #128\n   mov x2, #2\n   mov x3, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(futex)) "\n"
    SYSCALL_INSTRUCTION "\n"
    "mov x0, x4\n   b .Llock_take_arm64_contended\n"
    ".Llock_take_arm64_done:\n"
    ASM_RET
    ASM_END(lock_take)

    ASM_FUNC(lock_release)
    "adrp x1, threads_live\n   ldr x1, [x1, :lo12:threads_live]\n"
    "cbnz x1, .Llock_release_arm64_shared\n"
    "str wzr, [x0]\n"
    ASM_RET
    ".Llock_release_arm64_shared:\n"
    "ldxr w2, [x0]\n   stxr w3, wzr, [x0]\n"
    "cbnz w3, .Llock_release_arm64_shared\n"
    "cmp w2, #1\n   b.ne .Llock_release_arm64_wake\n"
    ASM_RET
    ".Llock_release_arm64_wake:\n"
    "mov x1, #129\n   mov x2, #1\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(futex)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(lock_release)

    ASM_FUNC(lock_try)
    "adrp x1, threads_live\n   ldr x1, [x1, :lo12:threads_live]\n"
    "cbnz x1, .Llock_try_arm64_shared\n"
    "ldr w2, [x0]\n   cbnz w2, .Llock_try_arm64_busy\n"
    "mov w2, #1\n   str w2, [x0]\n   mov w0, #1\n"
    ASM_RET
    ".Llock_try_arm64_busy:\n"
    "mov w0, #0\n"
    ASM_RET
    ".Llock_try_arm64_shared:\n"
    "mov w1, #1\n"
    ".Llock_try_arm64_claim:\n"
    "ldxr w2, [x0]\n   cbnz w2, .Llock_try_arm64_lost\n"
    "stxr w3, w1, [x0]\n   cbnz w3, .Llock_try_arm64_claim\n"
    "mov w0, #1\n"
    ASM_RET
    ".Llock_try_arm64_lost:\n"
    "clrex\n   mov w0, #0\n"
    ASM_RET
    ASM_END(lock_try)

    ASM_FUNC(thread_wait)
    "mov w2, w1\n   mov x1, #128\n   mov x3, xzr\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(futex)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(thread_wait)

    ASM_FUNC(thread_wake)
    "mov w2, w1\n   mov x1, #129\n"
    "mov " SYSCALL_NUMBER_REGISTER ", #" LINUX_RUNTIME_TEXT(syscall(futex)) "\n"
    SYSCALL_INSTRUCTION "\n"
    ASM_RET
    ASM_END(thread_wake)
);
#elif RISCV64
__asm__(
    ASM_SECTION
    //      lr.w/sc.w for the compare-and-swap and amoswap.w for the exchange:
    //      the A extension is part of the documented floor.
    ASM_FUNC(lock_take)
    "lla t0, threads_live\n   ld t0, 0(t0)\n"
    "bnez t0, .Llock_take_riscv64_shared\n"
    "li t1, 1\n   sw t1, 0(a0)\n"
    ASM_RET
    ".Llock_take_riscv64_shared:\n"
    "li t1, 1\n"
    ".Llock_take_riscv64_claim:\n"
    "lr.w t2, (a0)\n   bnez t2, .Llock_take_riscv64_contended\n"
    "sc.w t3, t1, (a0)\n   bnez t3, .Llock_take_riscv64_claim\n"
    ASM_RET
    ".Llock_take_riscv64_contended:\n"
    "li t1, 2\n   amoswap.w t2, t1, (a0)\n"
    "beqz t2, .Llock_take_riscv64_done\n"
    "mv t4, a0\n   li a1, 128\n   li a2, 2\n   li a3, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(futex)) "\n   ecall\n"
    "mv a0, t4\n   j .Llock_take_riscv64_contended\n"
    ".Llock_take_riscv64_done:\n"
    ASM_RET
    ASM_END(lock_take)

    ASM_FUNC(lock_release)
    "lla t0, threads_live\n   ld t0, 0(t0)\n"
    "bnez t0, .Llock_release_riscv64_shared\n"
    "sw zero, 0(a0)\n"
    ASM_RET
    ".Llock_release_riscv64_shared:\n"
    "amoswap.w t2, zero, (a0)\n   li t1, 1\n"
    "bne t2, t1, .Llock_release_riscv64_wake\n"
    ASM_RET
    ".Llock_release_riscv64_wake:\n"
    "li a1, 129\n   li a2, 1\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(futex)) "\n   ecall\n"
    ASM_RET
    ASM_END(lock_release)

    ASM_FUNC(lock_try)
    "lla t0, threads_live\n   ld t0, 0(t0)\n"
    "bnez t0, .Llock_try_riscv64_shared\n"
    "lw t2, 0(a0)\n   bnez t2, .Llock_try_riscv64_busy\n"
    "li t1, 1\n   sw t1, 0(a0)\n   li a0, 1\n"
    ASM_RET
    ".Llock_try_riscv64_busy:\n"
    "li a0, 0\n"
    ASM_RET
    ".Llock_try_riscv64_shared:\n"
    "li t1, 1\n"
    ".Llock_try_riscv64_claim:\n"
    "lr.w t2, (a0)\n   bnez t2, .Llock_try_riscv64_busy\n"
    "sc.w t3, t1, (a0)\n   bnez t3, .Llock_try_riscv64_claim\n"
    "li a0, 1\n"
    ASM_RET
    ASM_END(lock_try)

    ASM_FUNC(thread_wait)
    "mv a2, a1\n   li a1, 128\n   li a3, 0\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(futex)) "\n   ecall\n"
    ASM_RET
    ASM_END(thread_wait)

    ASM_FUNC(thread_wake)
    "mv a2, a1\n   li a1, 129\n"
    "li a7, " LINUX_RUNTIME_TEXT(syscall(futex)) "\n   ecall\n"
    ASM_RET
    ASM_END(thread_wake)
);
#endif

#undef LINUX_RUNTIME_TEXT
#undef LINUX_RUNTIME_TEXT_INNER

#endif
