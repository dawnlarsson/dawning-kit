/*
        Experimental C standard library

        <signal.h>: dispositions, masks, and the half of setjmp that was missing

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SIGNAL
#define STANDARD_MODERN_C_STANDARD_SIGNAL

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason src/standard/text.c gives at the same point: core.c
        includes the umbrella, library.c sets KERNEL_MODE from __MODULE__, and
        a module that pulled a second struct sigaction in beside the one
        <linux/signal.h> already has would not compile.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        What this file needs under it, said out loud so it can be included
        from a test before the umbrella grows a line for it.

        errno and the three return-shape wrappers belong to the error family
        and every routine here reports failure through them. The include is
        guarded on that file's own guard, so the umbrella including error.c
        first and a test including this file directly both end up with exactly
        one copy.

        setjmp.inc is next door and holds jump_mark, jump_to_mark and the
        thirty two slot jump_state that sigsetjmp extends. It guards itself
        too, and src/standard/text.c may already have read it.
*/
#ifndef STANDARD_MODERN_C_STANDARD_ERROR
#include "error.c"
#endif

#include "../platform/setjmp.inc"

/*
        The numbers, which are the same on all three machines and are checked
        rather than remembered.

        Linux gives x86_64, arm64 and riscv64 the identical signal numbering:
        the asm-generic list, plus SIGSTKFLT at sixteen, which x86 contributed
        and the other two inherited. That was confirmed by building a program
        against each toolchain's own <signal.h> and printing all thirty one --
        every number below came back the same on all three.

        Each one is wrapped in its own ifndef and not in one block ifndef.
        library.c already defines SIGTRAP, SIGKILL, SIGSTOP and SIGCHLD for
        its own use and src/standard/stdlib.c defines SIGABRT for abort, so a
        single guard around the whole list would silently drop the other
        twenty six the first time any one of them was already present. Four
        families colliding on names that were each correct alone is the
        failure this arrangement exists to not have.

        SIGIOT, SIGPOLL and SIGCLD are the older spellings of SIGABRT, SIGIO
        and SIGCHLD and cost nothing to carry; code being ported in uses them
        and a missing one is a compile error a long way from its cause.
*/
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGIOT
#define SIGIOT 6
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGSTKFLT
#define SIGSTKFLT 16
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCLD
#define SIGCLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif
#ifndef SIGURG
#define SIGURG 23
#endif
#ifndef SIGXCPU
#define SIGXCPU 24
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 25
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 26
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGIO
#define SIGIO 29
#endif
#ifndef SIGPOLL
#define SIGPOLL 29
#endif
#ifndef SIGPWR
#define SIGPWR 30
#endif
#ifndef SIGSYS
#define SIGSYS 31
#endif
#ifndef SIGUNUSED
#define SIGUNUSED 31
#endif

/*
        The real-time range, which is a range and not a list.

        Linux reserves thirty two through sixty four for queued signals and
        gives the first few to whoever asks first; glibc takes thirty two and
        thirty three for its own thread cancellation and identity changes and
        hands out thirty four upwards. This library has no threads and takes
        none of them, so SIGRTMIN here is the kernel's thirty two rather than
        glibc's thirty four. That is a deliberate difference from glibc and it
        is the only one in this file's numbering; it is written down here
        because a program ported between the two and using SIGRTMIN+1 as a
        cookie would otherwise find the two libraries disagreeing about which
        signal it meant.
*/
#ifndef SIGRTMIN
#define SIGRTMIN 32
#endif
#ifndef SIGRTMAX
#define SIGRTMAX 64
#endif

//      Sixty four signals, numbered from one, so the highest is also the
//      count. NSIG is one past it, which is the spelling every header uses
//      for the bound of a loop over dispositions.
#define SIGNAL_HIGHEST 64

#ifndef NSIG
#define NSIG 65
#endif
#ifndef _NSIG
#define _NSIG 65
#endif

/*
        The flags, which are the kernel's own bit values and identical on the
        three machines -- again checked against each toolchain's
        <asm/signal.h> rather than recalled.

        SA_RESTORER is in this list and is not for callers. It is the bit that
        says a restorer address is present in the structure, this file sets it
        on x86_64 and only on x86_64, and it is cleared out of anything handed
        back to a caller so that a program reading a disposition it did not
        install does not see a flag it never set. riscv64 does not define it
        at all, which is why it is guarded and why nothing outside the one
        line that sets it may mention it.
*/
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x00000001
#endif
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x00000002
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x00000004
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0x40000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef SA_NOMASK
#define SA_NOMASK SA_NODEFER
#endif
#ifndef SA_ONESHOT
#define SA_ONESHOT SA_RESETHAND
#endif

//      Only x86_64 has the field this names. Spelled with the family key on
//      the internal side so that nothing here depends on whether a sibling
//      header already defined the standard name.
#define SIGNAL_KERNEL_RESTORER 0x04000000

//      What sigprocmask's first argument may be.
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif

/*
        A disposition is a function pointer, and the three that are not
        functions.

        Zero and one are addresses the kernel reads as "the default action"
        and "throw it away", and minus one is never a disposition -- it is
        what signal returns when it could not install one. All three are
        written as a pointer type so that a caller can compare against them
        without a cast and so that the comparison is against the same thing
        the field holds.
*/
typedef fn(address_to signal_handler)(b32 number);
typedef fn(address_to signal_restorer)(void);

#ifndef SIG_DFL
#define SIG_DFL ((signal_handler)0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((signal_handler)1)
#endif
#ifndef SIG_ERR
#define SIG_ERR ((signal_handler) - 1)
#endif

/*
        The restorer and the sigsetjmp stub, which are instructions and live
        next door for the reason that file sets out at length.

        It is read here, before anything that uses it, because
        signal_action_change fills in the restorer address on x86_64 and
        cannot do that with a name it has not seen. Everything the .inc needs
        is already in scope: jump_state and DEAD_END from setjmp.inc above,
        the syscall table and MOONWATER_NUMBER from library.c.
*/
#include "../platform/signal.inc"

/*
        A set of signals, in the shape a program that was written against
        glibc already believes it has.

        This is the load-bearing decision in the file and it deserves the
        whole paragraph. There are two candidate layouts. The kernel's
        sigset_t is eight bytes, one bit per signal, and every rt_ system call
        below is told so by a trailing size argument that is always eight.
        glibc's sigset_t is a hundred and twenty eight bytes -- sixteen words,
        a thousand and twenty four bits -- of which it uses the first sixty
        four and leaves the rest alone. The kernel size is what the calls
        want; the glibc size is what a caller's header says a sigset_t is.

        This file uses the glibc size. The reason is the one thing this
        library is short of: the names it exports are real symbols with no C
        declaration, and the whole point of attaching the standard spellings
        is that an object compiled against real headers can be linked against
        this. Such an object declares sigset_t as a hundred and twenty eight
        bytes, allocates that many on its stack, and hands the address to
        sigemptyset. If sigemptyset here believed a set was eight bytes it
        would still work; if sigaction here believed a struct sigaction was
        the kernel's thirty two bytes it would read the caller's mask out of
        the wrong place and the whole file would be a quiet no-op for the one
        argument it exists to carry. Matching the caller costs a copy of eight
        bytes at each call and buys the only compatibility that matters.

        So: the public shapes are glibc's, the private ones are the kernel's,
        and the translation between them is one field assignment in each
        direction, written out below where it can be read.

        Confirmed on all three toolchains rather than assumed: sizeof
        sigset_t is 128, sizeof struct sigaction is 152, and its fields are at
        0, 8, 136 and 144 on x86_64, arm64 and riscv64 alike.
*/
#define SIGNAL_SET_WORDS 16
#define SIGNAL_KERNEL_SET_BYTES 8

typedef struct signal_set
{
        positive words[SIGNAL_SET_WORDS];
} signal_set;

/*
        Whether a number can name a signal at all.

        One through sixty four. Zero is not a signal -- kill takes it as "test
        whether the process exists" and sigaddset must refuse it -- and
        anything above sixty four has no bit. glibc answers EINVAL for both
        ends and so does this; the boundary was checked against glibc on all
        three machines, where sigaddset(64) succeeds and sigaddset(65) does
        not.
*/
static bool signal_number_bad(b32 number)
{
        return number < 1 || number > SIGNAL_HIGHEST;
}

/*
        The five set operations, which are a bit each.

        There is no library routine to reach for here and this is one of the
        places where a loop would be the wrong thing rather than the slow
        thing: every signal that exists lives in the first word, so the whole
        of each operation is a shift and one bitwise instruction. memory_zero
        does clear the other fifteen words, and it is used rather than a
        written-out loop because the size is a literal at the call site and
        the umbrella folds it to straight line stores.

        Clearing all sixteen words rather than only the first is what makes a
        set from here comparable, byte for byte, with a set a program built
        for itself. Recent glibc touches only the first word and leaves the
        rest as whatever was on the stack, which is defensible and is also why
        two sets holding the same signals can differ in a hundred and twenty
        bytes of it. Zeroing costs nothing measurable and removes a whole
        class of confusing comparison.
*/
b32 signal_set_empty(signal_set address_to set)
{
        memory_zero(set, sizeof(signal_set));

        return 0;
}

/*
        Every signal, and this library takes none of them for itself.

        glibc's sigfillset leaves thirty two and thirty three out, because
        NPTL uses those two to cancel threads and to change credentials across
        them, and a program that blocked them would break its own runtime.
        There are no threads here and nothing reserved, so a full set is full.
        The difference is visible if the two libraries are compared byte for
        byte and it is deliberate: copying a reservation this library does not
        have would be a lie in the one direction that is hard to find later.
*/
b32 signal_set_fill(signal_set address_to set)
{
        memory_zero(set, sizeof(signal_set));

        set->words[0] = (positive) - 1;

        return 0;
}

b32 signal_set_add(signal_set address_to set, b32 number)
{
        if (signal_number_bad(number))
        {
                errno = EINVAL;
                return -1;
        }

        set->words[0] |= (positive)1 << (number - 1);

        return 0;
}

b32 signal_set_remove(signal_set address_to set, b32 number)
{
        if (signal_number_bad(number))
        {
                errno = EINVAL;
                return -1;
        }

        set->words[0] &= ~((positive)1 << (number - 1));

        return 0;
}

b32 signal_set_has(const signal_set address_to set, b32 number)
{
        if (signal_number_bad(number))
        {
                errno = EINVAL;
                return -1;
        }

        return (b32)((set->words[0] >> (number - 1)) & 1);
}

/*
        The structure the kernel actually reads, which is not the one above
        and is not the same on all three machines.

        Linux builds struct sigaction out of two conditionals. The handler and
        the flags come first everywhere. A restorer field follows on
        architectures that define __ARCH_HAS_SA_RESTORER, and the mask comes
        last. x86_64 and arm64 define it, so their mask is at twenty four and
        the structure is thirty two bytes. riscv64 does not, so its mask is at
        sixteen and the structure is twenty four.

        That is exactly the trap this file was written to avoid walking into.
        A single four-word layout compiles on all three and works on all
        three as long as the third word is zero -- which is why the shell next
        door gets away with one, since it only ever installs SIG_IGN, SIG_DFL
        or a handler with an empty mask. The moment sa_mask carries anything,
        a four-word layout writes it into the restorer slot on riscv64 and the
        kernel reads zero: handlers still run, they still return, and the
        blocking the caller asked for silently does not happen. A test that
        only checks that a handler runs and returns passes with this bug in
        it, on the one machine of the three that has it.

        Verified rather than believed: preprocessing <asm/signal.h> with each
        of the three toolchains shows SA_RESTORER and __ARCH_HAS_SA_RESTORER
        defined for x86_64 and aarch64 and absent for riscv64. And the kernel
        does not object to being told otherwise -- a program that hands
        riscv64 the four word layout with the restorer bit set gets a zero
        back from rt_sigaction, its handler runs, and its mask is whatever
        eight bytes were in the restorer slot. That was measured, not
        imagined, and it is why the shape below is decided at compile time and
        checked at compile time rather than trusted.

        The condition the kernel actually uses is __ARCH_HAS_SA_RESTORER, not
        "x86_64 or arm64", and those two happen to be the same thing on every
        machine this library builds for. A fourth architecture arriving must
        be checked against that name in its own <asm/signal.h> rather than
        added to the list below by resemblance.
*/
#if X64 || ARM64
#define SIGNAL_KERNEL_ACTION_BYTES 32
#else
#define SIGNAL_KERNEL_ACTION_BYTES 24
#endif

typedef struct signal_kernel_action
{
        signal_handler handler;
        positive flags;

#if X64 || ARM64
        signal_restorer restorer;
#endif

        positive mask;
} signal_kernel_action;

/*
        Three sizes this file's correctness rests on, made into compile
        errors.

        A negative array bound is the portable way to say "stop" at
        preprocessing time, and these three are worth saying it about because
        every one of them was established by a separate probe program rather
        than by anything in this file. If a compiler ever packs or pads these
        differently, the failure without the check is not a build error: it is
        a mask read out of the wrong offset, on one architecture, at run time,
        with the handler still running and returning correctly.

        128 and 152 are glibc's sigset_t and struct sigaction on all three
        toolchains. 32 and 24 are the kernel's struct sigaction with and
        without the restorer slot.
*/
typedef char signal_set_is_the_size_a_caller_believes
        [(sizeof(signal_set) == 128) ? 1 : -1];

typedef char signal_kernel_action_is_the_size_the_kernel_reads
        [(sizeof(signal_kernel_action) == SIGNAL_KERNEL_ACTION_BYTES) ? 1 : -1];

/*
        The full disposition, in the caller's shape.

        Field for field this is glibc's struct sigaction: the handler union
        first, then the mask, then the flags, then the restorer. The order is
        not the kernel's and is not chosen here -- it is whatever a program's
        own <signal.h> says, and agreeing with that is the entire reason this
        type exists separately from the one above.

        There is one union in C's version, between a handler taking a number
        and a handler taking a number, a siginfo and a context. Both are
        pointers to code and the kernel is told which by SA_SIGINFO, so this
        holds the first spelling and a caller installing a three-argument
        handler casts. A union of two function pointer types would be the same
        eight bytes with two names, and the cast is the honest way to say that
        the flag and the pointer have to agree.
*/
typedef struct signal_action
{
        signal_handler handler;
        signal_set mask;
        b32 flags;
        signal_restorer restorer;
} signal_action;

typedef char signal_action_is_the_size_a_caller_believes
        [(sizeof(signal_action) == 152) ? 1 : -1];

/*
        sigaction, and the translation in both directions.

        The flags are widened through p32 and not through b32, because
        SA_RESETHAND is 0x80000000 and a signed widening of it would set every
        one of the top thirty two bits in the word the kernel reads. That is
        the kind of thing that works on a test that never uses the flag.

        On the way out the restorer is filled in on x86_64 only and the flag
        that announces it is set at the same moment, so the two can never
        disagree. On the way back that same flag is cleared from what the
        caller is told, along with the restorer field, because neither was
        the caller's: a program that installs a handler, reads the disposition
        back, and compares it against what it asked for should find them
        equal.

        A caller passing null for wanted is asking only to read, and one
        passing null for previous is asking only to write; both are passed
        straight through as null so the kernel does the deciding, and neither
        structure is copied when it is not going to be looked at.
*/
b32 signal_action_change(b32 number, const signal_action address_to wanted,
                         signal_action address_to previous)
{
        signal_kernel_action asked;
        signal_kernel_action had;
        b32 outcome;

        memory_zero(address_of asked, sizeof asked);
        memory_zero(address_of had, sizeof had);

        if (wanted)
        {
                asked.handler = wanted->handler;
                asked.flags = (positive)(p32)wanted->flags;
                asked.mask = wanted->mask.words[0];

#if X64
                asked.flags |= (positive)SIGNAL_KERNEL_RESTORER;
                asked.restorer = signal_return_trampoline;
#endif
        }

        outcome = error_whole(system_call_4(
                syscall(rt_sigaction), (positive)number,
                wanted ? (positive)address_of asked : 0,
                previous ? (positive)address_of had : 0,
                SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        if (previous)
        {
                memory_zero(previous, sizeof(signal_action));

                previous->handler = had.handler;
                previous->flags =
                        (b32)(p32)(had.flags &
                                   ~(positive)SIGNAL_KERNEL_RESTORER);
                previous->mask.words[0] = had.mask;
        }

        return 0;
}

/*
        signal, which is sigaction with an opinion.

        There have been two of these historically. System V's resets the
        disposition to the default before the handler runs and does not
        restart an interrupted system call, so a handler that wants to keep
        catching has to reinstall itself and every read has to be retried by
        hand. BSD's does neither, and glibc's signal is BSD's. This is BSD's,
        for the same reason: a program ported in was written against whatever
        its libc did, and its libc was glibc.

        So SA_RESTART and an empty mask -- empty because the kernel blocks the
        signal being delivered for the duration of its own handler unless
        SA_NODEFER says otherwise, and sa_mask is for blocking the OTHER
        signals a handler must not be interrupted by.

        The previous handler is read back in the same call and returned, which
        is the one thing signal does that a caller usually keeps.
*/
signal_handler signal_handle(b32 number, signal_handler handler)
{
        signal_action wanted;
        signal_action had;

        memory_zero(address_of wanted, sizeof wanted);
        memory_zero(address_of had, sizeof had);

        wanted.handler = handler;
        wanted.flags = SA_RESTART;

        if (signal_action_change(number, address_of wanted, address_of had) < 0)
                return SIG_ERR;

        return had.handler;
}

/*
        raise, sent to this thread and not to this process.

        kill would deliver to the process, which means any thread that has the
        signal unblocked may take it -- and for a signal a program raises at
        itself, another thread taking it is never what was meant. tgkill names
        the thread. src/standard/stdlib.c's abort says the same thing about
        SIGABRT and reaches for the same call, and there is only one process
        and one thread in a spark binary today, so the difference is currently
        invisible and will stop being invisible the day there is a second
        thread.

        Neither getpid nor gettid can fail, so both are unwrapped, exactly as
        the error family's getpid is.
*/
b32 signal_raise(b32 number)
{
        b32 process = (b32)system_call(syscall(getpid));
        b32 thread = (b32)system_call(syscall(gettid));

        return error_whole(system_call_3(syscall(tgkill), (positive)process,
                                         (positive)thread, (positive)number));
}

/*
        The mask, which is the one place the eight byte kernel set and the
        hundred and twenty eight byte caller set meet.

        Reading is into a single word and writing is out of one. The rest of
        the caller's set is not consulted on the way in -- there are no
        signals there to consult -- and is zeroed on the way out, so what
        comes back is a set that compares equal to one built by hand holding
        the same signals.

        how is not checked here. The kernel checks it and answers EINVAL, and
        checking it twice would mean this file having an opinion about which
        values are legal that could drift from the kernel's. Confirmed against
        glibc: an out of range how gives -1 and EINVAL from both.
*/
b32 signal_mask_change(b32 how, const signal_set address_to wanted,
                       signal_set address_to previous)
{
        positive asked = wanted ? wanted->words[0] : 0;
        positive had = 0;
        b32 outcome;

        outcome = error_whole(system_call_4(
                syscall(rt_sigprocmask), (positive)how,
                wanted ? (positive)address_of asked : 0,
                previous ? (positive)address_of had : 0,
                SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        if (previous)
        {
                memory_zero(previous, sizeof(signal_set));
                previous->words[0] = had;
        }

        return 0;
}

/*
        sigsuspend, which is the only routine here whose success is a failure.

        It installs a mask, waits, runs whatever handler arrives, restores the
        old mask and returns. It always returns -1 with EINTR, because the
        only way out is a signal, and POSIX says so rather than inventing a
        success value for it. A caller that checks the return against -1 and
        reports an error is a caller with a bug, and every libc has the same
        shape here.

        A null mask means the empty mask -- suspend with nothing blocked --
        and that is the one place in this file where null is a value rather
        than an absence. Everywhere else a null structure means "not
        supplied", because there is another argument doing the work; here the
        mask IS the work and there is nothing for an absence to mean. glibc
        dereferences the null and the program stops, which is defensible and
        is not more useful. Written down because it is the only pointer in
        this file whose contract is not the one the others have.
*/
b32 signal_mask_suspend(const signal_set address_to mask)
{
        positive asked = mask ? mask->words[0] : 0;

        return error_whole(system_call_2(syscall(rt_sigsuspend),
                                         (positive)address_of asked,
                                         SIGNAL_KERNEL_SET_BYTES));
}

//      What has arrived and is being held back because it is blocked.
b32 signal_mask_pending(signal_set address_to into)
{
        positive had = 0;
        b32 outcome;

        outcome = error_whole(system_call_2(syscall(rt_sigpending),
                                            (positive)address_of had,
                                            SIGNAL_KERNEL_SET_BYTES));

        if (outcome < 0)
                return -1;

        memory_zero(into, sizeof(signal_set));
        into->words[0] = had;

        return 0;
}

/*
        A timer, because two of the three machines have no alarm.

        alarm is an old x86 system call, number thirty seven, and the
        asm-generic table that arm64 and riscv64 both use does not carry it at
        all -- there is no syscall_linux_arm64_alarm to reach for and a
        version written against the x86 number would not compile on the other
        two, which is the good outcome. setitimer exists on all three and
        ITIMER_REAL is the same clock alarm arms, so alarm is setitimer with
        the repeat interval left at zero, which is what glibc does on exactly
        the same machines and for exactly the same reason.

        The four fields are two timevals, repeat first and first-fire second,
        and they are spelled out rather than reusing the clock family's
        timeval so that this file can be included before that one. Nothing is
        gained by sharing a structure of two longs across an ordering
        dependency.

        The value handed back is what was left of a previous alarm, in whole
        seconds, rounded UP when there were microseconds on the end. Rounding
        down would let alarm(1) followed immediately by alarm(0) report zero
        seconds remaining, which reads as "there was no alarm" when there
        certainly was one. glibc rounds up here and this rounds up with it.
*/
#define SIGNAL_TIMER_REAL 0

typedef struct signal_interval
{
        bipolar repeat_seconds;
        bipolar repeat_microseconds;
        bipolar first_seconds;
        bipolar first_microseconds;
} signal_interval;

p32 signal_alarm(p32 seconds)
{
        signal_interval wanted;
        signal_interval had;

        memory_zero(address_of wanted, sizeof wanted);
        memory_zero(address_of had, sizeof had);

        wanted.first_seconds = (bipolar)seconds;

        if (error_whole(system_call_3(syscall(setitimer), SIGNAL_TIMER_REAL,
                                      (positive)address_of wanted,
                                      (positive)address_of had)) < 0)
                return 0;

        return (p32)had.first_seconds + (had.first_microseconds ? 1 : 0);
}

/*
        pause, which is also not a system call on two of the three.

        Number thirty four on x86_64 and absent from the asm-generic table.
        The stand-in is the one glibc uses on those machines: read the mask
        that is in force and suspend with it, which blocks exactly what was
        already blocked and waits.

        It is used on all three rather than only on the two that need it. The
        alternative -- the real call on x86_64 and this everywhere else --
        would mean the one routine in this file whose behaviour was written
        twice, and the difference between the two is nothing a program can
        observe: a signal that arrives in the window between reading the mask
        and suspending runs its handler and then this waits forever, and a
        signal that arrives just before the real pause does the same.
*/
b32 signal_wait(void)
{
        positive mask = 0;

        system_call_4(syscall(rt_sigprocmask), SIG_BLOCK, 0,
                      (positive)address_of mask, SIGNAL_KERNEL_SET_BYTES);

        return error_whole(system_call_2(syscall(rt_sigsuspend),
                                         (positive)address_of mask,
                                         SIGNAL_KERNEL_SET_BYTES));
}

/*
        The half of sigsetjmp that is not a jump.

        Called from the stub with the state and the flag still in the
        registers they arrived in. Slot 26 records whether a mask was wanted,
        because siglongjmp has to know without being told again, and slot 27
        holds it.

        Both slots are written even when no mask was asked for. A jump_state
        is an automatic array in most callers and holds whatever was on the
        stack; leaving slot 26 alone would mean a sigsetjmp with a zero second
        argument being read later as one that saved a mask, and restoring
        eight bytes of stack rubbish as the process's signal mask is a failure
        with no plausible symptom.

        The mask is read with SIG_BLOCK and a null set, which is how every
        libc asks "what is blocked right now" -- blocking nothing changes
        nothing and the old value comes back through the third argument.
*/
KEEP fn signal_jump_save(jump_state state, b32 save_mask)
{
        positive mask = 0;

        state[SIGNAL_JUMP_SAVED] = save_mask ? 1 : 0;
        state[SIGNAL_JUMP_MASK] = 0;

        if (!save_mask)
                return;

        system_call_4(syscall(rt_sigprocmask), SIG_BLOCK, 0,
                      (positive)address_of mask, SIGNAL_KERNEL_SET_BYTES);

        state[SIGNAL_JUMP_MASK] = mask;
}

/*
        siglongjmp, which unlike its partner is ordinary C.

        The asymmetry is worth stating because it looks like an oversight.
        sigsetjmp had to be assembly because jump_mark records the stack of
        whoever called it, and a C wrapper would have interposed a frame that
        is dead by the time the jump comes back. jump_to_mark records nothing
        and returns to nobody -- it moves the stack pointer to the mark and
        continues there -- so a C wrapper's frame is simply abandoned, which
        is what would have happened to it anyway.

        The mask goes back before the jump and not after, because after there
        is no after: jump_to_mark does not return here. Restoring first also
        means the code at the mark starts with the mask the mark was taken
        under, which is the entire promise sigsetjmp's second argument makes.

        A state saved with a zero second argument leaves the mask exactly as
        it is, which is _setjmp's behaviour and is what a program that did not
        ask for the syscall is paying nothing for.
*/
fn signal_jump_to_mark(jump_state state, b32 value)
{
        if (state[SIGNAL_JUMP_SAVED])
        {
                positive mask = state[SIGNAL_JUMP_MASK];

                system_call_4(syscall(rt_sigprocmask), SIG_SETMASK,
                              (positive)address_of mask, 0,
                              SIGNAL_KERNEL_SET_BYTES);
        }

        jump_to_mark(state, value);
}

/*
        The names a C program knows these by, as second labels on the same
        addresses, exactly as library.c attaches strlen to string_length.

        kill is not in this list and must not be added to it. The error family
        already defines kill, as a static C function with the POSIX name
        itself, so there is no prose routine here to attach and a .set naming
        it would be a second definition of a symbol the assembler has already
        seen in this translation unit.

        __sigsetjmp is beside sigsetjmp because glibc's header makes sigsetjmp
        a macro over __sigsetjmp, so an object compiled against real headers
        has a relocation against the underscored spelling and nothing against
        the plain one. Both are the same address here.
*/
__asm__(
    ASM_ALIAS(signal,      signal_handle)
    ASM_ALIAS(raise,       signal_raise)
    ASM_ALIAS(sigaction,   signal_action_change)
    ASM_ALIAS(sigemptyset, signal_set_empty)
    ASM_ALIAS(sigfillset,  signal_set_fill)
    ASM_ALIAS(sigaddset,   signal_set_add)
    ASM_ALIAS(sigdelset,   signal_set_remove)
    ASM_ALIAS(sigismember, signal_set_has)
    ASM_ALIAS(sigprocmask, signal_mask_change)
    ASM_ALIAS(sigsuspend,  signal_mask_suspend)
    ASM_ALIAS(sigpending,  signal_mask_pending)
    ASM_ALIAS(alarm,       signal_alarm)
    ASM_ALIAS(pause,       signal_wait)
    ASM_ALIAS(sigsetjmp,   signal_jump_mark)
    ASM_ALIAS(__sigsetjmp, signal_jump_mark)
    ASM_ALIAS(siglongjmp,  signal_jump_to_mark)
);

/*
        And, unusually for this tree, the declarations to go with them.

        Everywhere else in this library a standard name is a symbol with no
        prototype: nm shows memcpy and strlen and sqrt in the object, and
        &memcpy still does not compile, because a .set tells the linker a name
        and tells the compiler nothing. That gap is the single largest thing
        between this library and an ordinary C program, and it is not this
        family's to close in general.

        It is closable for one family at a time at the cost of four lines,
        which is what follows. A declaration with no definition is not an
        error; the .set above supplies the definition at link time. So code in
        this tree can write sigaction(SIGINT, &wanted, 0) and have it compile
        and link, which is the first place in this library where a POSIX name
        is usable as a name rather than only as a symbol.

        The types are this file's, not <signal.h>'s, and that is the honest
        thing rather than a limitation: a translation unit that included the
        real header would take its declarations from there and never see
        these. The one turned into a name a caller must not shadow is
        sigaction, which is a function here and a structure in the real
        header; nothing in this tree declares struct sigaction, and a file
        that needs to should not include this one.

        STANDARD_SIGNAL_NO_DECLARATIONS turns the block off in one line if a
        later family arrives with its own spelling of any of these.
*/
#ifndef STANDARD_SIGNAL_NO_DECLARATIONS

extern signal_handler signal(b32 number, signal_handler handler);
extern b32 raise(b32 number);
extern b32 sigaction(b32 number, const signal_action address_to wanted,
                     signal_action address_to previous);
extern b32 sigemptyset(signal_set address_to set);
extern b32 sigfillset(signal_set address_to set);
extern b32 sigaddset(signal_set address_to set, b32 number);
extern b32 sigdelset(signal_set address_to set, b32 number);
extern b32 sigismember(const signal_set address_to set, b32 number);
extern b32 sigprocmask(b32 how, const signal_set address_to wanted,
                       signal_set address_to previous);
extern b32 sigsuspend(const signal_set address_to mask);
extern b32 sigpending(signal_set address_to into);
extern p32 alarm(p32 seconds);
extern b32 pause(void);
extern b32 sigsetjmp(jump_state state, b32 save_mask) __attribute__((returns_twice));
extern fn siglongjmp(jump_state state, b32 value) DEAD_END;

#endif // STANDARD_SIGNAL_NO_DECLARATIONS

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SIGNAL
