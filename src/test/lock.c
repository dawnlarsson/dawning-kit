/*
        Experimental C standard library

        the lock: that it excludes, that it sleeps, and what it costs when
        nobody is contending for it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        This lane exists to answer one question the tree has been putting off:
        what would it cost to make the allocator, errno and the streams safe
        for a second thread. Three families say in their own comments that
        they are not, and none of those comments carries a number.

        So this does four things, in the order of how much they are worth.

        First it proves the lock excludes, with two real threads incrementing
        one counter a hundred thousand times each. Without a lock that
        arithmetic loses increments on every machine here; with it the total
        is exact, every run.

        Second it proves the slow path is real. A lock that spins would pass
        the first test and would be a different thing entirely, so one thread
        holds the lock across a sleep while another blocks on it, and the
        blocked thread's own observation -- that the holder's flag was already
        set when it got in -- is what says it waited rather than spun through.

        Third it proves the same algorithm works across processes over a
        MAP_SHARED page, which is a second implementation of the same idea and
        catches anything that depended on one address space.

        Fourth it measures. That is the deliverable: an uncontended
        take-and-release pair against the cost of the malloc and free it would
        wrap. The numbers are printed rather than checked, because a timing is
        not a pass or a fail, and only the number from a native run means
        anything -- qemu-user does not model a bus lock and makes an atomic
        look almost free.

        THE TRAMPOLINE, WHICH IS WHY THERE IS ASSEMBLY HERE

        A thread is a clone with CLONE_VM and a stack, and the child cannot
        return into C from the syscall that made it: the kernel gives it the
        stack pointer that was asked for, and the C frame the caller was in
        the middle of building is behind the old one. Every threading library
        solves this the same way, with a per-architecture stub that lands on
        the new stack, picks up an entry point and an argument from the top of
        it, calls, and traps exit when the call returns.

        It is in the test and not in src/standard/lock.c on purpose.
        src/standard is ordinary C by rule and this cannot be; when threads
        are really shipped this belongs in src/platform beside the other
        three-way code. src/test/wait_retry.c has the same shape for the same
        reason -- a signal restorer it needs and no shipped family should own.
*/
#include "../compiler_memory.c"

#if defined(KERNEL_MODE) || defined(STANDARD_NO_PLATFORM)
#error "the lock is a Linux userspace primitive"
#endif

static positive checks;
static positive failures;

#define check(name, condition)                                   \
        do {                                                     \
                checks++;                                        \
                if (!(condition))                                \
                {                                                \
                        failures++;                              \
                        string_format(log, "  FAIL " name "\n"); \
                }                                                \
        } while (0)

#define LOCK_TEXT_INNER(value) #value
#define LOCK_TEXT(value) LOCK_TEXT_INNER(value)

/*
        The flags that make a thread rather than a process.

        CLONE_VM is the one that matters -- one address space, so the lock
        word both sides touch is the same memory. SIGHAND and THREAD are what
        make it a thread of this process rather than a sibling: THREAD
        requires SIGHAND, SIGHAND requires VM, and together they mean the
        child shares the process id and is not waitable. FS and FILES are
        there so that the child sees this process's working directory and
        descriptors, which is what a thread is expected to have.

        Deliberately no CLONE_SETTLS. Nothing the child runs touches a
        __thread object, and installing a thread block is a separate question
        this lane does not answer; the errno probe that does is elsewhere.
*/
#define LOCK_CLONE_VM 0x00000100
#define LOCK_CLONE_FS 0x00000200
#define LOCK_CLONE_FILES 0x00000400
#define LOCK_CLONE_SIGHAND 0x00000800
#define LOCK_CLONE_THREAD 0x00010000
#define LOCK_CLONE_SYSVSEM 0x00040000

/*
        SYSVSEM is in the set for a reason found by running this: the flags
        the kernel accepts and the flags qemu-user accepts are not the same
        set. Linux is happy with VM|FS|FILES|SIGHAND|THREAD and makes a thread
        of it. qemu-user refuses that combination with EINVAL, because its
        do_fork tests the flags for exact equality against the set glibc's
        NPTL uses, and NPTL always passes CLONE_SYSVSEM as well -- it shares
        the System V semaphore undo list, which threads of one process are
        supposed to.

        So the flag that made two of the three targets work is one the native
        machine did not need. On Linux it is correct rather than merely
        tolerated: a thread that does not share the undo list would have its
        semaphore adjustments undone when it exits, which is not what a thread
        means. Getting it right for the emulator got it right for the kernel.
*/
#define LOCK_CLONE_THREAD_FLAGS                                     \
        (LOCK_CLONE_VM | LOCK_CLONE_FS | LOCK_CLONE_FILES |         \
         LOCK_CLONE_SIGHAND | LOCK_CLONE_THREAD | LOCK_CLONE_SYSVSEM)

/*
        lock_thread_spawn(entry, argument, stack_top)

        Writes entry and argument into the top sixteen bytes of the stack,
        clones onto what is left, and in the child pulls both back out and
        calls. Returns the child's thread id in the parent, or the kernel's
        negative answer; the child never returns from here at all.

        stack_top must be sixteen byte aligned, which every one of the three
        wants and two of them fault without.
*/
bipolar lock_thread_spawn(fn(address_to entry)(address_any),
                          address_any argument, address_any stack_top);

#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(lock_thread_spawn)
    //  rdi entry, rsi argument, rdx stack top.
    "sub $16, %rdx\n"
    "mov %rdi, 0(%rdx)\n"
    "mov %rsi, 8(%rdx)\n"
    //  clone(flags, stack, parent_tid, child_tid, tls) with rdi rsi rdx r10 r8.
    "mov %rdx, %rsi\n"
    "mov $" LOCK_TEXT(LOCK_CLONE_THREAD_FLAGS) ", %edi\n"
    "xor %edx, %edx\n"
    "xor %r10d, %r10d\n"
    "xor %r8d, %r8d\n"
    "mov $" LOCK_TEXT(syscall(clone)) ", %eax\n"
    "syscall\n"
    "test %rax, %rax\n"
    "jnz 1f\n"
    //  The child lands here with rsp at the two words written above.
    "pop %rax\n"
    "pop %rdi\n"
    "call *%rax\n"
    "xor %edi, %edi\n"
    "mov $" LOCK_TEXT(syscall(exit)) ", %eax\n"
    "syscall\n"
    "1:\n"
    ASM_RET
    ASM_END(lock_thread_spawn)
);
#elif ARM64
__asm__(
    ASM_SECTION
    ASM_FUNC(lock_thread_spawn)
    //  x0 entry, x1 argument, x2 stack top.
    "sub x2, x2, #16\n"
    "str x0, [x2]\n"
    "str x1, [x2, #8]\n"
    //  clone(flags, stack, parent_tid, tls, child_tid) with x0..x4.
    "mov x1, x2\n"
    "movz x0, #0x0f00\n"
    "movk x0, #0x0005, lsl #16\n"
    "mov x2, #0\n"
    "mov x3, #0\n"
    "mov x4, #0\n"
    "mov x8, #" LOCK_TEXT(syscall(clone)) "\n"
    "svc #0\n"
    "cbnz x0, 1f\n"
    "ldr x9, [sp]\n"
    "ldr x0, [sp, #8]\n"
    "add sp, sp, #16\n"
    "blr x9\n"
    "mov x0, #0\n"
    "mov x8, #" LOCK_TEXT(syscall(exit)) "\n"
    "svc #0\n"
    "1:\n"
    ASM_RET
    ASM_END(lock_thread_spawn)
);
#else
__asm__(
    ASM_SECTION
    ASM_FUNC(lock_thread_spawn)
    //  a0 entry, a1 argument, a2 stack top.
    "addi a2, a2, -16\n"
    "sd a0, 0(a2)\n"
    "sd a1, 8(a2)\n"
    //  clone(flags, stack, parent_tid, tls, child_tid) with a0..a4.
    "mv a1, a2\n"
    "li a0, " LOCK_TEXT(LOCK_CLONE_THREAD_FLAGS) "\n"
    "li a2, 0\n"
    "li a3, 0\n"
    "li a4, 0\n"
    "li a7, " LOCK_TEXT(syscall(clone)) "\n"
    "ecall\n"
    "bnez a0, 1f\n"
    "ld t0, 0(sp)\n"
    "ld a0, 8(sp)\n"
    "addi sp, sp, 16\n"
    "jalr t0\n"
    "li a0, 0\n"
    "li a7, " LOCK_TEXT(syscall(exit)) "\n"
    "ecall\n"
    "1:\n"
    ASM_RET
    ASM_END(lock_thread_spawn)
);
#endif

/*
        Sixty-four kilobytes of stack per thread, in .bss, sixteen byte
        aligned because two of the three fault otherwise.

        Static rather than mapped because a test that allocates its own thread
        stacks is testing the allocator as well, and the allocator is one of
        the things this lane is measuring.
*/
#define LOCK_THREADS 2
#define LOCK_STACK_BYTES 65536

static p8 lock_stacks[LOCK_THREADS][LOCK_STACK_BYTES]
        __attribute__((aligned(16)));

static address_any lock_stack_top(positive which)
{
        return (address_any)(lock_stacks[which] + LOCK_STACK_BYTES);
}

/*
        A join, which a CLONE_THREAD child cannot be given by the kernel:
        wait4 refuses a thread of the same group with ECHILD, because the
        parent is not its parent, it is its sibling. So the child sets a flag
        as the last thing it does and this spins on it, yielding rather than
        burning, which is exactly what a real join does underneath before it
        has a futex to wait on.

        volatile because the compiler is entitled to assume a plain global
        does not change inside a loop that does not write it, and would hoist
        the load out and spin forever.
*/
static fn lock_join(volatile positive address_to done, positive wanted)
{
        while (address_to done < wanted)
                lock_yield();
}

//      -- what the threads run --------------------------------------------

static lock lock_guard = lock_start;
static volatile positive lock_finished = 0;

/*
        volatile, and it is not decoration.

        A plain positive here would be kept in a register for the whole of a
        hundred thousand iterations and written back once, which turns the
        unlocked control below into a single store and makes it lose nothing
        -- the test would then be proving that the compiler optimised the race
        away rather than that the lock prevented one. Written volatile, every
        turn is a real load, add and store, which is what a shared counter is
        and what the lock has to make atomic.
*/
static volatile positive lock_counter = 0;

/*
        A gate, so that both threads are running before either starts
        counting.

        Without it the unlocked control serialises by accident: spawning the
        second thread takes longer than the first thread's whole unlocked loop,
        so the two never overlap and the count comes out exact for a reason
        that has nothing to do with correctness. The gate costs one flag and
        makes the control mean what it says.
*/
static volatile positive lock_go = 0;

static fn lock_wait_for_go(void)
{
        while (lock_go == 0)
                lock_yield();
}

#define LOCK_TURNS 100000

/*
        The control, which is what keeps the test above from being vacuous.

        Two threads incrementing one counter without a lock lose increments,
        because a read-modify-write of a plain object is three instructions
        and a second thread fits between any two of them. Running it proves
        that the counting test is measuring the lock and not merely measuring
        that two threads happened not to overlap.

        It is printed and not checked. Losing an increment is overwhelmingly
        likely and it is not certain: an unlucky schedule where the two
        threads never overlap would give the exact total, and a lane that
        failed on that would fail for being right.
*/
static fn lock_racing_thread(address_any argument)
{
        positive turn = 0;

        (void)argument;

        lock_wait_for_go();

        while (turn < LOCK_TURNS)
        {
                lock_counter++;
                turn++;
        }

        atomic_add(address_of lock_finished, 1);
}

static fn lock_counting_thread(address_any argument)
{
        positive turn = 0;

        (void)argument;

        lock_wait_for_go();

        while (turn < LOCK_TURNS)
        {
                lock_take(address_of lock_guard);
                lock_counter++;
                lock_release(address_of lock_guard);
                turn++;
        }

        atomic_add(address_of lock_finished, 1);
}

/*
        The sleep test.

        The holder takes the lock, says so, sleeps long enough that the waiter
        cannot plausibly still be running, and releases. The waiter takes the
        lock and records what the holder's flag said at the moment it got in.
        A lock that excluded would give 1; a lock that did not would usually
        give 0, because the waiter would have gone straight through while the
        holder slept.

        The nap is fifty milliseconds, which is long against a context switch
        and short against a test run.
*/
static volatile positive lock_holder_inside = 0;
static volatile positive lock_waiter_saw = 0;
static volatile positive lock_waiter_ran = 0;

static fn lock_nap(positive nanoseconds)
{
        positive duration[2];

        duration[0] = nanoseconds / 1000000000ULL;
        duration[1] = nanoseconds % 1000000000ULL;

        system_call_2(syscall(nanosleep), (positive)address_of duration, 0);
}

static fn lock_holding_thread(address_any argument)
{
        (void)argument;

        lock_take(address_of lock_guard);
        lock_holder_inside = 1;
        lock_nap(50000000);
        lock_holder_inside = 2;
        lock_release(address_of lock_guard);

        atomic_add(address_of lock_finished, 1);
}

static fn lock_waiting_thread(address_any argument)
{
        (void)argument;

        //      Let the holder get in first. Without this the waiter may take
        //      the lock before the holder ever tries and the test would be
        //      measuring nothing.
        while (lock_holder_inside == 0)
                lock_yield();

        lock_take(address_of lock_guard);
        lock_waiter_saw = lock_holder_inside;
        lock_release(address_of lock_guard);

        lock_waiter_ran = 1;
        atomic_add(address_of lock_finished, 1);
}

//      -- the clock, for the measurement ----------------------------------

#define LOCK_CLOCK_MONOTONIC 1

static positive lock_now(void)
{
        positive when[2] = {0, 0};

        system_call_2(syscall(clock_gettime), LOCK_CLOCK_MONOTONIC,
                      (positive)address_of when);

        return when[0] * 1000000000ULL + when[1];
}

//      Somewhere for the measured loops to put their answers, so that nothing
//      in them is dead code the optimiser is entitled to delete.
static volatile positive lock_sink = 0;

/*
        The measurement.

        Three loops over the same count. The first is malloc and free of one
        size class, which after a warm-up hits the free list every time and is
        the allocator's fast path and nothing else -- a loop that grew the
        heap would be measuring mmap. The second is the same loop with an
        uncontended take and release around it, which is exactly what a
        thread-safe allocator would do. The third is the pair on its own, so
        the cost can be quoted without the allocator in front of it.

        Printed, not checked. A timing that failed a lane would fail it on a
        busy machine.
*/
#ifndef LOCK_MEASURE_TURNS
#define LOCK_MEASURE_TURNS 300000
#endif

#define LOCK_MEASURE_SIZE 64

static fn lock_measure(void)
{
        positive turn;
        positive started;
        positive bare;
        positive wrapped;
        positive alone;
        lock quiet = lock_start;

        //      Warm the class so that every timed iteration is a free list
        //      pop and a free list push.
        turn = 0;

        while (turn < 1000)
        {
                address_any block = malloc(LOCK_MEASURE_SIZE);

                lock_sink += (positive)block;
                free(block);
                turn++;
        }

        started = lock_now();
        turn = 0;

        while (turn < LOCK_MEASURE_TURNS)
        {
                address_any block = malloc(LOCK_MEASURE_SIZE);

                lock_sink += (positive)block;
                free(block);
                turn++;
        }

        bare = lock_now() - started;

        started = lock_now();
        turn = 0;

        while (turn < LOCK_MEASURE_TURNS)
        {
                address_any block;

                lock_take(address_of quiet);
                block = malloc(LOCK_MEASURE_SIZE);
                lock_sink += (positive)block;
                free(block);
                lock_release(address_of quiet);
                turn++;
        }

        wrapped = lock_now() - started;

        started = lock_now();
        turn = 0;

        while (turn < LOCK_MEASURE_TURNS)
        {
                lock_take(address_of quiet);
                lock_sink++;
                lock_release(address_of quiet);
                turn++;
        }

        alone = lock_now() - started;

        string_format(log, "  measure: %p turns of malloc+free\n",
                      (positive)LOCK_MEASURE_TURNS);
        string_format(log, "  measure: bare      %p ns total, %p ps each\n",
                      bare, bare * 1000 / LOCK_MEASURE_TURNS);
        string_format(log, "  measure: locked    %p ns total, %p ps each\n",
                      wrapped, wrapped * 1000 / LOCK_MEASURE_TURNS);
        string_format(log, "  measure: lock only %p ns total, %p ps each\n",
                      alone, alone * 1000 / LOCK_MEASURE_TURNS);

        if (bare)
                string_format(log,
                              "  measure: the lock adds %p percent to malloc+free\n",
                              wrapped > bare ? (wrapped - bare) * 100 / bare : 0);
}

//      -- the cross-process half ------------------------------------------

/*
        The same algorithm over a page two processes share.

        MAP_SHARED with MAP_ANONYMOUS gives a page that survives a fork and is
        one object in both processes, which is what a futex needs to be keyed
        by inode rather than by address space. The lock taken here is the
        shared spelling for that reason, and mixing the two is the mistake
        src/standard/lock.c warns about: a private futex on this page would
        put the two processes in different wait queues and neither would ever
        wake the other.
*/
#define LOCK_MAP_SHARED 1
#define LOCK_MAP_ANONYMOUS 0x20
#define LOCK_PROTECT_READ_WRITE 3
#define LOCK_SHARED_TURNS 20000

typedef struct
{
        lock guard;
        positive counter;
} lock_shared_page;

static fn lock_across_processes(void)
{
        lock_shared_page address_to page;
        b32 child;
        positive raw = 0;
        positive turn;

        page = (lock_shared_page address_to)mmap(
                null, 4096, LOCK_PROTECT_READ_WRITE,
                LOCK_MAP_SHARED | LOCK_MAP_ANONYMOUS, -1, 0);

        if (page == MAP_FAILED)
        {
                check("a shared page could be mapped", false);
                return;
        }

        page->guard.word = LOCK_FREE;
        page->counter = 0;

        log_flush();

        child = fork();

        if (child < 0)
        {
                check("the second process started", false);
                munmap((address_any)page, 4096);
                return;
        }

        if (child == 0)
        {
                turn = 0;

                while (turn < LOCK_SHARED_TURNS)
                {
                        lock_take_shared(address_of page->guard);
                        page->counter++;
                        lock_release_shared(address_of page->guard);
                        turn++;
                }

                _exit(0);
        }

        turn = 0;

        while (turn < LOCK_SHARED_TURNS)
        {
                lock_take_shared(address_of page->guard);
                page->counter++;
                lock_release_shared(address_of page->guard);
                turn++;
        }

        system_wait4_retry(child, address_of raw, 0, null);

        check("two processes lost no increments",
              page->counter == LOCK_SHARED_TURNS * 2);
        check("the shared lock came back free",
              page->guard.word == LOCK_FREE);

        munmap((address_any)page, 4096);
}

//      -- the lane --------------------------------------------------------

b32 main(void)
{
        bipolar first;
        bipolar second;

        //
        //      The state machine, with nobody else in the process.
        //
        {
                lock quiet = lock_start;

                check("a fresh lock is free", lock_state(address_of quiet) == LOCK_FREE);
                check("try on a free lock succeeds", lock_try(address_of quiet));
                check("a taken lock reads held",
                      lock_state(address_of quiet) == LOCK_HELD);
                check("try on a held lock fails", !lock_try(address_of quiet));
                lock_release(address_of quiet);
                check("a released lock is free again",
                      lock_state(address_of quiet) == LOCK_FREE);

                lock_take(address_of quiet);
                check("take leaves it held",
                      lock_state(address_of quiet) == LOCK_HELD);
                lock_release(address_of quiet);
                check("release leaves it free",
                      lock_state(address_of quiet) == LOCK_FREE);
        }

        //
        //      Two threads, one counter.
        //
        lock_finished = 0;
        lock_counter = 0;
        lock_go = 0;

        first = lock_thread_spawn(lock_counting_thread, null, lock_stack_top(0));
        second = lock_thread_spawn(lock_counting_thread, null, lock_stack_top(1));
        lock_go = 1;

        check("the first thread started", first > 0);
        check("the second thread started", second > 0);
        check("the two threads have different identities", first != second);

        if (first > 0 && second > 0)
        {
                lock_join(address_of lock_finished, 2);

                check("two threads lost no increments",
                      lock_counter == LOCK_TURNS * 2);
                check("the lock came back free",
                      lock_state(address_of lock_guard) == LOCK_FREE);

                string_format(log, "  counted %p of %p under the lock\n",
                              lock_counter, (positive)(LOCK_TURNS * 2));
        }

        //
        //      The same two threads with the lock taken away, so that the
        //      number above means something.
        //
        lock_finished = 0;
        lock_counter = 0;
        lock_go = 0;

        first = lock_thread_spawn(lock_racing_thread, null, lock_stack_top(0));
        second = lock_thread_spawn(lock_racing_thread, null, lock_stack_top(1));
        lock_go = 1;

        if (first > 0 && second > 0)
        {
                lock_join(address_of lock_finished, 2);

                string_format(log, "  counted %p of %p with no lock at all\n",
                              lock_counter, (positive)(LOCK_TURNS * 2));
        }

        //
        //      That a waiter really waits.
        //
        lock_finished = 0;
        lock_holder_inside = 0;
        lock_waiter_saw = 0;
        lock_waiter_ran = 0;

        first = lock_thread_spawn(lock_holding_thread, null, lock_stack_top(0));
        second = lock_thread_spawn(lock_waiting_thread, null, lock_stack_top(1));

        if (first > 0 && second > 0)
        {
                lock_join(address_of lock_finished, 2);

                check("the waiter ran", lock_waiter_ran == 1);
                check("the waiter got in only after the holder left",
                      lock_waiter_saw == 2);
        }
        else
                check("both threads started for the sleep test", false);

        //
        //      The same algorithm across two address spaces.
        //
        lock_across_processes();

        //
        //      And what it costs when there is nobody to wait for.
        //
        lock_measure();

        string_format(log, "\n%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures != 0;
}
