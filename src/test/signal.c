#include "../compiler_memory.c"

/*
        <signal.h>, and the half of setjmp that was missing, on three machines.

        src/standard/signal.c is included directly when the umbrella has not
        already pulled it in, so this test builds and runs before the merge
        that adds that line to src/compiler_memory.c and stays correct after
        it: the file guards itself, and the guard is what this asks about.

        Every case is in src/test/signal_cases.inc, which
        src/test/signal_reference.c builds against the host's glibc. The two
        are supposed to print the same verdict, and a difference is a real
        disagreement about behaviour rather than a difference of opinion about
        what to test.

        The three things this lane is really for, and none of them are visible
        in a test that only checks that a handler ran:

          a handler RETURNS. On x86_64 nothing returns from a handler unless
          sa_restorer points at a two instruction trampoline that calls
          rt_sigreturn, and the failure without one is SIGSEGV rather than a
          wrong answer. Building this file with that flag removed dies at 139
          on the first raise.

          sa_mask ARRIVES. riscv64's kernel struct sigaction has no restorer
          slot, so its mask is eight bytes earlier than on the other two.
          Building this file with the x86_64 layout forced on all three fails
          exactly one check on exactly riscv64, and every other check still
          passes.

          the MASK survives the jump. A handler that leaves by longjmp never
          returns, so the signal it was handling stays blocked forever unless
          the mark wrote the mask down. sigsetjmp with a one is what writes it
          down and siglongjmp is what puts it back.
*/
#ifndef STANDARD_MODERN_C_STANDARD_SIGNAL
#include "../standard/signal.c"
#endif

#include "counted.inc"

/*
        What the cases are written against.

        One name per routine and per type, so the case list mentions neither
        this library's spelling nor glibc's and can be compiled against
        either.
*/
typedef signal_set sig_set;
typedef signal_action sig_action;
typedef signal_handler sig_handler;
typedef jump_state sig_jump_state;

#define sig_errno errno
#define sig_zero(place, size) memory_zero((place), (size))

#define sig_set_empty(set) signal_set_empty(set)
#define sig_set_fill(set) signal_set_fill(set)
#define sig_set_add(set, number) signal_set_add((set), (number))
#define sig_set_remove(set, number) signal_set_remove((set), (number))
#define sig_set_has(set, number) signal_set_has((set), (number))

#define sig_action_change(number, wanted, previous)                           \
        signal_action_change((number), (wanted), (previous))
#define sig_handle(number, handler) signal_handle((number), (handler))
#define sig_raise(number) signal_raise(number)
#define sig_mask_change(how, wanted, previous)                                \
        signal_mask_change((how), (wanted), (previous))
#define sig_mask_suspend(mask) signal_mask_suspend(mask)
#define sig_mask_pending(into) signal_mask_pending(into)
#define sig_alarm(seconds) signal_alarm(seconds)
#define sig_wait() signal_wait()

#define sig_handler_of(action) ((action).handler)
#define sig_mask_of(action) ((action).mask)
#define sig_flags_of(action) ((action).flags)

//      Only the bits a caller is allowed to set. SA_RESTORER is not one of
//      them, and glibc hands it back on x86_64 where this library does not.
#define SIG_CALLER_FLAGS                                                      \
        (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK | SA_RESTART | \
         SA_NODEFER | SA_RESETHAND)

#define sig_flags_asked(action) (sig_flags_of(action) & SIG_CALLER_FLAGS)

#define sig_jump_mark(state, save) signal_jump_mark((state), (save))
#define sig_jump_to_mark(state, value) signal_jump_to_mark((state), (value))

#include "signal_cases.inc"

b32 main(void)
{
        sig_cases_all();

        /*
                The two questions glibc answers differently on purpose, asked
                here in the shape this library answers them.

                A full set is full. glibc leaves thirty two and thirty three
                out because NPTL cancels threads and changes credentials with
                them and a program that blocked them would break its own
                runtime. There are no threads here and nothing reserved, so
                filling a set fills it.
        */
        {
                signal_set set;

                signal_set_fill(address_of set);

                check("a full set holds the first reserved signal",
                      signal_set_has(address_of set, 32) == 1);
                check("and the second",
                      signal_set_has(address_of set, 33) == 1);
                check("and the last signal there is",
                      signal_set_has(address_of set, 64) == 1);
        }

        /*
                A disposition read back holds only what the caller put in it.

                x86_64 needs a restorer and this library supplies one, so the
                flags the kernel holds have SA_RESTORER in them on that
                machine and not on the other two. Handing that back would make
                a program that saves a disposition and compares it against
                what it asked for disagree with itself on one architecture out
                of three. glibc does hand it back -- a read back there answers
                0x14000000 on x86_64 and 0x10000000 on arm64 and riscv64 --
                and this is the deliberate divergence.
        */
        {
                signal_action wanted;
                signal_action previous;

                memory_zero(address_of wanted, sizeof wanted);
                memory_zero(address_of previous, sizeof previous);

                wanted.handler = SIG_IGN;
                wanted.flags = SA_RESTART;

                signal_action_change(SIGUSR1, address_of wanted, null);
                signal_action_change(SIGUSR1, null, address_of previous);

                check("the flags read back are exactly the flags asked for",
                      previous.flags == SA_RESTART);

                signal_action_change(SIGUSR1, null, null);
                signal_handle(SIGUSR1, SIG_DFL);
        }

        /*
                The queued signals, which this library hands out from thirty
                two and glibc hands out from thirty four.

                glibc keeps thirty two and thirty three for NPTL and its
                SIGRTMIN is therefore two above the kernel's. There are no
                threads here and nothing reserved, so SIGRTMIN is the kernel's
                own, and the claim that makes that safe rather than merely
                different is the one checked below: both are ordinary signals
                that take a handler, deliver, and return.

                It is checked rather than only documented because a constant
                that disagrees with glibc and has no test behind it is a trap
                for whoever ports the next program in.
        */
        {
                check("the queued range starts where the kernel starts it",
                      SIGRTMIN == 32);
                check("and ends at the last signal there is", SIGRTMAX == 64);

                sig_first_ran = 0;
                check("the first reserved signal takes a handler",
                      signal_handle(SIGRTMIN, sig_note_first) != SIG_ERR);
                check("and delivers", signal_raise(SIGRTMIN) == 0 &&
                                              sig_first_ran == SIGRTMIN);

                sig_first_ran = 0;
                check("so does the second",
                      signal_handle(SIGRTMIN + 1, sig_note_first) != SIG_ERR);
                check("and delivers", signal_raise(SIGRTMIN + 1) == 0 &&
                                              sig_first_ran == SIGRTMIN + 1);

                signal_handle(SIGRTMIN, SIG_DFL);
                signal_handle(SIGRTMIN + 1, SIG_DFL);
        }

#ifndef STANDARD_SIGNAL_NO_DECLARATIONS
        /*
                And the thing this family can do that no other family in this
                library can yet: call the POSIX names as names.

                Everywhere else a standard spelling is a symbol with no
                prototype -- nm shows memcpy, and &memcpy does not compile.
                src/standard/signal.c declares its sixteen and lets the .set
                supply the bodies, so the lines below are ordinary C calling
                sigemptyset and sigprocmask by those names.

                Under the same guard the declarations themselves carry, so
                that turning the block off in the library -- which is the
                remedy if a later family arrives with its own prototype for
                any of the sixteen -- turns this off with it and leaves a lane
                that still builds and still checks everything else. The rest
                of this file goes through the prose names and does not care.
        */
        {
                signal_set set;
                signal_set had;

                check("sigemptyset by name", sigemptyset(address_of set) == 0);
                check("sigaddset by name",
                      sigaddset(address_of set, SIGUSR2) == 0);
                check("sigismember by name",
                      sigismember(address_of set, SIGUSR2) == 1);
                check("sigprocmask by name",
                      sigprocmask(SIG_BLOCK, address_of set, address_of had) ==
                              0);
                check("raise by name on a blocked signal",
                      raise(SIGUSR2) == 0);
                check("sigpending by name",
                      sigpending(address_of had) == 0 &&
                              sigismember(address_of had, SIGUSR2) == 1);
                check("signal by name", signal(SIGUSR2, SIG_IGN) == SIG_DFL);
                check("sigprocmask by name again",
                      sigprocmask(SIG_UNBLOCK, address_of set, null) == 0);
                check("alarm by name", alarm(0) == 0);
                signal(SIGUSR2, SIG_DFL);
        }
#endif // STANDARD_SIGNAL_NO_DECLARATIONS

        return test_report(null);
}
