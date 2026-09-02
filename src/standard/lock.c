/*
        Experimental C standard library

        lock: one word, one compare-and-swap, and a futex only when somebody
        is actually waiting

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_LOCK
#define STANDARD_MODERN_C_STANDARD_LOCK

/*
        Why this file exists at all, in a library that says everywhere else
        that it is single threaded.

        Three families in src/standard are wrong the moment a second thread
        exists, and each of them says so in its own words. The allocator says
        it plainly under a heading called THREADS: two threads in malloc at
        the same time corrupt the free lists, and the whole of the mutable
        state is six file-scope objects touched inside allocator_take and
        memory_give. error.c keeps errno in a plain .bss object because
        __thread faults in a spark binary with nothing to set the thread
        register. stream.c has no per-stream lock, so two threads writing to
        one FILE interleave into its buffer.

        None of those is fixable by argument. Each one turns on a number: what
        does the lock cost when nobody is contending for it. That number is
        the whole reason this file is here, and it is a small enough file that
        it can be deleted again if the answer is no.

        WHAT THIS IS NOT

        It is not a threading library. Nothing here creates a thread, joins
        one, or has an opinion about how one should be made. Creating a thread
        means a clone with CLONE_VM and a stack the child lands on, and the
        child cannot return into C from the syscall that made it -- its stack
        pointer has moved and the frame it was building is behind it -- so a
        thread entry needs a per-architecture assembly trampoline, which
        belongs in src/platform beside the other three-way code and not in a
        file of ordinary C. src/test/lock.c has one, because a test may reach
        for assembly where a shipped family may not, and that trampoline is
        what proves the code below excludes.

        WHAT IT IS

        Drepper's three-state futex lock, which is the smallest correct mutex
        anybody has found and is what glibc's own low-level lock still is
        underneath. The word is 0 free, 1 held with nobody waiting, 2 held
        with somebody possibly waiting. The uncontended path is exactly one
        compare-and-swap to take and one exchange to release, with no trap in
        either, which is the shape that makes the measurement meaningful: what
        a lock costs a single-threaded program is those two instructions and
        nothing else.

        The third state is what buys that. A two-state lock has to ask the
        kernel to wake somebody on every release, because a release cannot
        tell whether anyone is asleep. The 2 records that a waiter was seen,
        so a release that finds a 1 knows there is nobody to wake and returns
        without a trap. A release that finds a 2 when the last waiter has
        already given up wakes nobody and has wasted one syscall, which is the
        only way this errs and it errs in the harmless direction.

        THE ATOMICS ARE ALREADY HERE

        library.c line 446 onward defines atomic_add, atomic_exchange and
        atomic_compare_exchange over the __sync builtins, and the inventory's
        floor paragraph names the A extension as part of what riscv64 is built
        against. Verified rather than assumed: a program built with the shipped
        three lines -- plain gcc, aarch64-linux-gnu-gcc -mno-outline-atomics,
        riscv64-linux-gnu-gcc -march=rv64imafd_zicsr_zicntr -- links with no
        libatomic and runs the compare-exchange, the exchange and the add
        correctly on all three. So the honest answer to "does this library
        have atomics" is yes, inline, at the documented floor, and this file
        needed to invent nothing.

        WHAT IT COSTS, WHICH IS THE POINT

        Measured on a Ryzen 9 9950X, native, five rounds of five million and
        the best of them, with the shipped -O2 spark link. Picoseconds per
        operation:

              empty loop                        194
              one compare-and-swap             3609
              lock take + release              7231
              malloc(64) + free                3374
              lock + malloc + free + unlock    8897
              fputc to a buffered stream       3073
              lock + fputc + unlock            9224

        Read the third line against the second: a take and a release are two
        locked read-modify-writes and cost exactly twice one, with nothing
        else in them. Read it against the fourth and the sixth and the answer
        to the question this file exists for falls out. An uncontended lock
        costs 7.2 nanoseconds. malloc and free together cost 3.4. fputc costs
        3.1. So locking the allocator makes malloc and free 2.6 times slower
        and locking a stream makes fputc 3.0 times slower, and neither of
        those is a rounding error.

        Only the native figures mean anything. The same program under
        qemu-aarch64 and qemu-riscv64 reports a lock that looks four to five
        times cheaper relative to malloc, because qemu-user translates an
        atomic into ordinary host instructions and models no bus lock and no
        cache line at all. Those runs prove the code is correct on those
        machines. They do not measure it.

        AND WHAT THE ANSWER PROBABLY IS

        Not this lock on every call. The number that changes the decision is
        one more that was measured: a load of a "how many threads are live"
        counter and a predictable branch, with the atomics taken only when the
        answer is more than one, costs 245 picoseconds rather than 7231. That
        turns +164 percent on malloc into +6.6 percent, and +200 percent on
        fputc into +12 percent. It is what glibc did for years under the name
        SINGLE_THREAD_P, and it is nearly free because the branch is perfectly
        predicted in a program that never spawns anything.

        It is deliberately not built into lock_take here, because it is only
        correct if something owns thread creation and increments that counter
        before the second thread can run. Nothing in this tree does yet. When
        something does, the elision belongs in this file and the branch
        belongs in these two functions -- and the measurement above is what
        says it is worth the care it needs.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        The three states, named rather than spelled as numbers at eight sites.
*/
#define LOCK_FREE 0
#define LOCK_HELD 1
#define LOCK_WAITED 2

/*
        futex's two operations and the flag that says the word is private to
        one address space.

        FUTEX_WAIT is 0 and FUTEX_WAKE is 1 in the old multiplexed futex
        syscall, which is number 202 on x86_64 and 98 on the asm-generic
        table that arm64 and riscv64 share. src/platform/syscall.inc also
        carries futex_wait and futex_wake as syscalls 455 and 454 -- those are
        the new unmultiplexed entries added in 6.7, and a kernel older than
        that answers them ENOSYS. The old one has been there since 2.6 and is
        not going anywhere, so it is what this uses.

        PRIVATE is the difference between a lock two threads share and a lock
        two processes share, and it is not cosmetic. A private futex is keyed
        by the address space and the address; a shared one is keyed by the
        page's inode and offset. Two threads use the private key and save the
        kernel a page lookup on every wait. Two processes over a MAP_SHARED
        page have different address spaces, so a private key would put them in
        different wait queues and neither would ever wake the other -- a lock
        that appears to work until it deadlocks. That is why there are two
        public spellings below and not one with a comment.
*/
#define LOCK_FUTEX_WAIT 0
#define LOCK_FUTEX_WAKE 1
#define LOCK_FUTEX_PRIVATE 128

/*
        A lock is one 32 bit word, and it is a struct rather than a bare b32
        so that a caller cannot pass an unrelated integer by accident and so
        that the field can grow -- an owner id for recursion, a waiter count
        for fairness -- without touching a call site.

        futex requires the word be 32 bits and naturally aligned. A struct
        with one b32 in it is both, on all three.

        Zero is unlocked, so a lock in .bss is ready without an initialiser
        and lock_start is only needed where a local one is being written down.
*/
typedef struct
{
        b32 word;
} lock;

#define lock_start \
        {          \
                LOCK_FREE  \
        }

/*
        The two traps, which are the only place this file touches the kernel.

        The wait is allowed to return for reasons that are not a release: a
        signal, or the word having changed before the kernel got to look at
        it, which is what the val argument is for and is why the answer is
        discarded. Every caller re-checks the word in a loop, so a spurious
        return costs one more turn and cannot be wrong.
*/
#define lock_futex_wait(word, was, private)                                 \
        ((fn)system_call_6(syscall(futex), (positive)(word),                \
                           (positive)(LOCK_FUTEX_WAIT | (private)),          \
                           (positive)(was), 0, 0, 0))
#define lock_futex_wake(word, private)                                      \
        ((fn)system_call_6(syscall(futex), (positive)(word),                \
                           (positive)(LOCK_FUTEX_WAKE | (private)),          \
                           1, 0, 0, 0))

/*
        Take, which is one compare-and-swap when nobody else holds it.

        The fast path is the whole point and is written first so it reads that
        way: if the word is free, claim it and return, no trap, no loop, no
        second memory reference. Everything below that line runs only when the
        first swap lost.

        The slow path exchanges 2 in rather than testing first. That is
        deliberate and is the subtle half of Drepper's design. Testing for a 1
        and only then upgrading to a 2 has a window: the holder can release
        between the test and the upgrade, and then the sleeper waits on a word
        nobody will wake. An unconditional exchange to 2 cannot have that
        window, because the value it gets back is the truth at the instant it
        wrote: a 0 means the lock became free and is now held by us, and
        anything else means it is still held and is now marked as contended,
        which is exactly the state the release path tests for.

        The word is left at 2 when a waiter acquires it, so that thread's
        release does one futex_wake nobody needed. Correct, and cheaper than
        the bookkeeping that would avoid it.

        private is a constant at every call site below, so the or in the two
        traps folds and there is no branch on it.
*/
static fn lock_take_private(lock address_to it, b32 private)
{
        b32 was;

        if_common(atomic_compare_exchange(address_of it->word, LOCK_FREE,
                                          LOCK_HELD))
                return;

        was = atomic_exchange(address_of it->word, LOCK_WAITED);

        while (was != LOCK_FREE)
        {
                lock_futex_wait(address_of it->word, LOCK_WAITED, private);
                was = atomic_exchange(address_of it->word, LOCK_WAITED);
        }
}

/*
        Release, which is one exchange when nobody is waiting.

        An exchange rather than a compare-and-swap because there is nothing to
        compare against: the holder is releasing, so whatever the word says it
        is going to become 0, and the only question is what it said on the way
        out. A 1 means no waiter was ever recorded and there is nobody to
        wake. Anything else means at least one thread wrote a 2, and one of
        them gets woken.

        Waking exactly one and not all of them is the whole difference between
        a lock and a thundering herd: every woken thread but one would find
        the word held again and go straight back to sleep, having paid for a
        context switch to learn nothing.
*/
static fn lock_release_private(lock address_to it, b32 private)
{
        if_common(atomic_exchange(address_of it->word, LOCK_FREE) == LOCK_HELD)
                return;

        lock_futex_wake(address_of it->word, private);
}

/*
        Try, which never traps and never blocks.

        One compare-and-swap and the answer. This is the primitive
        ftrylockfile would be built on, and it is also what a caller that
        cannot afford to block -- a signal handler, a diagnostic path -- has
        to use instead of take.
*/
static bool lock_try(lock address_to it)
{
        return atomic_compare_exchange(address_of it->word, LOCK_FREE,
                                       LOCK_HELD) != 0;
}

//      Whether anybody holds it. A read of a word that another thread may be
//      writing, so it is a fact about the past and useful only for assertions
//      and for a test that wants to see the state machine move.
static b32 lock_state(lock address_to it)
{
        return it->word;
}

//      The two threads-in-one-process spellings, which is the case this
//      library will meet first.
#define lock_take(it) lock_take_private((it), LOCK_FUTEX_PRIVATE)
#define lock_release(it) lock_release_private((it), LOCK_FUTEX_PRIVATE)

//      The two across-processes spellings, for a lock that lives in a
//      MAP_SHARED page. Same algorithm, different futex key; see the note
//      above LOCK_FUTEX_PRIVATE for why mixing them silently deadlocks.
#define lock_take_shared(it) lock_take_private((it), 0)
#define lock_release_shared(it) lock_release_private((it), 0)

/*
        Which thread is asking, for a test that wants to prove two of them
        really are running and for an owner field if one is ever added.

        gettid rather than getpid: inside a CLONE_THREAD group every thread
        shares the process id and only the thread id tells them apart.
*/
static b32 lock_thread_identity(void)
{
        return (b32)system_call(syscall(gettid));
}

//      Give the rest of the run queue a turn without sleeping. Not used by
//      the lock -- it has a futex for that -- but a spin-until-flag join in a
//      caller wants it, and it is one line here rather than a raw trap there.
static fn lock_yield(void)
{
        system_call(syscall(sched_yield));
}

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_LOCK
