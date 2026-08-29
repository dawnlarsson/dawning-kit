#define _GNU_SOURCE

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
        The same cases, against the host's glibc.

        This is not built by the distribution and it links nothing the
        distribution ships. It exists so that every expectation in
        src/test/signal_cases.inc is checked against the implementation those
        routines are specified by, on the machine the tests run on -- an
        expectation that only src/standard/signal.c satisfies is an
        expectation that agrees with a bug.

        Build it beside the freestanding one and compare the verdicts:

            gcc -O2 -no-pie -w -o /tmp/signal.reference \
                src/test/signal_reference.c

        -no-pie for the same reason the string reference wants it: the case
        file takes the addresses of handlers and compares them against what a
        disposition read back, and a position independent executable would
        route some of those through the PLT so that two names for one function
        stop comparing equal.

        This waits a real second in the alarm case, exactly as the
        freestanding build does.
*/

static unsigned long long checks;
static unsigned long long failures;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        printf("  FAIL " name "\n");                    \
                }                                                       \
        } while (0)

typedef sigset_t sig_set;
typedef struct sigaction sig_action;
typedef void (*sig_handler)(int);
typedef sigjmp_buf sig_jump_state;

#define sig_errno errno
#define sig_zero(place, size) memset((place), 0, (size))

#define sig_set_empty(set) sigemptyset(set)
#define sig_set_fill(set) sigfillset(set)
#define sig_set_add(set, number) sigaddset((set), (number))
#define sig_set_remove(set, number) sigdelset((set), (number))
#define sig_set_has(set, number) sigismember((set), (number))

#define sig_action_change(number, wanted, previous)                           \
        sigaction((number), (wanted), (previous))
#define sig_handle(number, handler) signal((number), (handler))
#define sig_raise(number) raise(number)
#define sig_mask_change(how, wanted, previous)                                \
        sigprocmask((how), (wanted), (previous))
#define sig_mask_suspend(mask) sigsuspend(mask)
#define sig_mask_pending(into) sigpending(into)
#define sig_alarm(seconds) alarm(seconds)
#define sig_wait() pause()

#define sig_handler_of(action) ((action).sa_handler)
#define sig_mask_of(action) ((action).sa_mask)
#define sig_flags_of(action) ((action).sa_flags)

//      Only the bits a caller is allowed to set. glibc hands SA_RESTORER back
//      on the architectures that have one, and that bit is not a caller's.
#define SIG_CALLER_FLAGS                                                      \
        (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK | SA_RESTART | \
         SA_NODEFER | SA_RESETHAND)

#define sig_flags_asked(action) (sig_flags_of(action) & SIG_CALLER_FLAGS)

#define sig_jump_mark(state, save) sigsetjmp((state), (save))
#define sig_jump_to_mark(state, value) siglongjmp((state), (value))

#include "signal_cases.inc"

int main(void)
{
        sig_cases_all();

        /*
                The two questions this library answers differently on purpose,
                asked here in the shape glibc answers them, so that the
                divergence is written down as a passing check on both sides
                rather than as a count that does not match.

                glibc's sigfillset leaves thirty two and thirty three out.
                They are SIGCANCEL and SIGSETXID, NPTL's own, and a program
                that blocked them would break the runtime under it.
                src/standard/signal.c has no threads to protect and fills all
                sixty four.
        */
        {
                sigset_t set;

                sigfillset(&set);

                check("glibc keeps the first reserved signal out of a full set",
                      sigismember(&set, 32) == 0);
                check("and the second", sigismember(&set, 33) == 0);
                check("and the last signal there is still goes in",
                      sigismember(&set, 64) == 1);
        }

        /*
                A disposition read back carries glibc's own restorer bit on
                the architectures that have one.

                Not spelled SA_RESTORER, because riscv64's headers do not
                define it -- the bit is asked about by value.

                x86_64 is the only one of the three where it comes back. That
                was measured rather than assumed, and the first version of
                this file assumed arm64 as well and was corrected by its own
                failure: arm64 defines SA_RESTORER and would honour one, but
                glibc does not supply one there and lets the kernel point x30
                at the vDSO trampoline instead -- which is the same choice
                src/standard/signal.c makes. riscv64 has no such field at all.

                So glibc answers 0x14000000 on x86_64 and 0x10000000 on the
                other two. src/standard/signal.c clears the bit on all three,
                so that what a caller reads back is what a caller asked for
                and is the same number on every machine.
        */
        {
                struct sigaction wanted;
                struct sigaction previous;
                int leaked;

                memset(&wanted, 0, sizeof wanted);
                memset(&previous, 0, sizeof previous);

                wanted.sa_handler = SIG_IGN;
                wanted.sa_flags = SA_RESTART;
                sigemptyset(&wanted.sa_mask);

                sigaction(SIGUSR1, &wanted, NULL);
                sigaction(SIGUSR1, NULL, &previous);

                leaked = (previous.sa_flags & 0x04000000) != 0;

                check("the flags a caller may set came back",
                      (previous.sa_flags & SIG_CALLER_FLAGS) == SA_RESTART);
#if defined(__x86_64__)
                check("and glibc's own restorer bit came back with them",
                      leaked == 1);
#else
                check("and glibc supplied no restorer on this machine",
                      leaked == 0);
#endif

                signal(SIGUSR1, SIG_DFL);
        }

        printf("%llu checks, %llu failures\n", checks, failures);

        return failures ? 1 : 0;
}
