/*
        Experimental C standard library

        The cases for <signal.h>, and for the half of setjmp that was missing

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_TEST_SIGNAL_CASES
#define STANDARD_MODERN_C_TEST_SIGNAL_CASES

/*
        One list of cases, two libraries under it, exactly as
        src/test/strings_cases.inc arranges the same thing for <string.h>.

        src/test/signal.c builds this against the library, freestanding, on
        x86_64, arm64 and riscv64. src/test/signal_reference.c builds the same
        lines against the host's glibc. Both print the same verdict, and an
        expectation written here that only this library satisfies fails the
        reference build rather than quietly agreeing with a wrong
        implementation.

        Everything below is reached through sig_ names -- sig_set for a set of
        signals, sig_action for a disposition, one macro per routine -- so
        nothing here mentions sigprocmask or signal_mask_change and nothing
        here knows which library it is testing.

        The two places the libraries genuinely differ are kept OUT of this
        file and are checked separately in each host, because a shared case
        can only ask a question both can answer:

          signals 32 and 33. glibc reserves them for thread cancellation and
          identity changes and leaves them out of a full set; this library has
          no threads and fills all sixty four. So no case here asks what a
          full set contains at 32 or 33.

          SA_RESTORER on the way back. glibc hands the caller the restorer bit
          it set itself, so a disposition read back on x86_64 has 0x14000000
          where arm64 and riscv64 have 0x10000000. This library clears it, and
          answers 0x10000000 on all three. So the cases compare only the flags
          a caller is allowed to set, through sig_flags_asked.

        Signals 61 to 64 are also avoided throughout. qemu-user reserves the
        top few real-time signals for its own use and a mask set through it
        does not read back with them, which is an emulator artefact and not a
        disagreement about libraries.
*/

/*
        What the handlers write down.

        File scope and volatile, not locals. A handler runs between two
        arbitrary instructions of whatever it interrupted, so anything it
        touches has to be a real store to a real address rather than a value
        the compiler is entitled to keep in a register across the raise.
*/
static volatile int sig_first_ran;
static volatile int sig_second_ran;
static volatile int sig_second_seen_inside_first;
static volatile int sig_alarm_ran;
static volatile int sig_suspend_ran;
static volatile int sig_ignored_ran;

static sig_jump_state sig_place;

static void sig_note_first(int number)
{
        sig_first_ran = number;
}

/*
        Raises the second signal from inside the first, then looks to see
        whether it ran. That look is the whole of the sa_mask case: if the
        mask went where the kernel reads masks, the second handler cannot have
        run yet and the answer is zero.
*/
static void sig_note_first_and_raise(int number)
{
        sig_first_ran = number;
        sig_raise(SIGUSR2);
        sig_second_seen_inside_first = sig_second_ran;
}

static void sig_note_second(int number)
{
        sig_second_ran = number;
}

static void sig_note_alarm(int number)
{
        sig_alarm_ran = number;
}

static void sig_note_suspend(int number)
{
        sig_suspend_ran = number;
}

static void sig_note_ignored(int number)
{
        sig_ignored_ran = number;
}

//      Leaves through the mark rather than returning, which is the case
//      sigsetjmp's second argument exists for.
static void sig_leave_from_handler(int number)
{
        sig_first_ran = number;
        sig_jump_to_mark(sig_place, 1);
}

/*
        Every case starts from a known disposition and a known mask, so that
        one failing case cannot make the next one fail for a reason that is
        not its own.

        The mask is SET to empty rather than having the three signals used
        here unblocked out of it. Unblocking a list is enough today and stops
        being enough the moment a case is added that blocks a fourth signal --
        and the way that failure presents is the NEXT case hanging in
        sigsuspend or losing a raise, which reads as a bug in whatever was
        added last rather than in this function.
*/
static void sig_case_reset(void)
{
        sig_set none;

        sig_first_ran = 0;
        sig_second_ran = 0;
        sig_second_seen_inside_first = 0;
        sig_alarm_ran = 0;
        sig_suspend_ran = 0;
        sig_ignored_ran = 0;

        sig_set_empty(&none);
        sig_mask_change(SIG_SETMASK, &none, 0);

        sig_handle(SIGUSR1, SIG_DFL);
        sig_handle(SIGUSR2, SIG_DFL);
        sig_handle(SIGALRM, SIG_DFL);
        sig_alarm(0);
}

/*
        The five set operations, which are the only routines here that touch
        no kernel at all.

        The bounds are the interesting part. One and sixty four are signals;
        zero and sixty five are not, and both ends answer EINVAL. That is not
        obvious from the numbering -- sixty four signals numbered from one
        means the highest valid number equals the count, and an off by one in
        either direction is a set operation that silently touches the wrong
        bit or refuses a real signal.
*/
static void sig_case_set_operations(void)
{
        sig_set set;
        int number;
        int missing = 0;

        sig_set_empty(&set);

        for (number = 1; number <= 31; number++)
                if (sig_set_has(&set, number) != 0)
                        missing++;

        check("an empty set holds nothing from 1 to 31", missing == 0);

        sig_set_fill(&set);

        missing = 0;

        for (number = 1; number <= 31; number++)
                if (sig_set_has(&set, number) != 1)
                        missing++;

        for (number = 34; number <= 60; number++)
                if (sig_set_has(&set, number) != 1)
                        missing++;

        check("a full set holds every standard and queued signal",
              missing == 0);

        sig_set_empty(&set);

        check("adding one signal", sig_set_add(&set, SIGUSR1) == 0);
        check("the added signal is a member", sig_set_has(&set, SIGUSR1) == 1);
        check("no other signal came with it",
              sig_set_has(&set, SIGUSR2) == 0);
        check("removing it", sig_set_remove(&set, SIGUSR1) == 0);
        check("and then it is not a member",
              sig_set_has(&set, SIGUSR1) == 0);

        check("signal one is a signal", sig_set_add(&set, 1) == 0);
        check("and reads back", sig_set_has(&set, 1) == 1);
        check("signal sixty four is a signal", sig_set_add(&set, 64) == 0);
        check("and reads back", sig_set_has(&set, 64) == 1);

        sig_errno = 0;
        check("zero is not a signal to add", sig_set_add(&set, 0) == -1);
        check("and says why", sig_errno == EINVAL);

        sig_errno = 0;
        check("sixty five is not a signal to add",
              sig_set_add(&set, 65) == -1);
        check("and says why", sig_errno == EINVAL);

        sig_errno = 0;
        check("nor to remove", sig_set_remove(&set, 0) == -1);
        check("and says why", sig_errno == EINVAL);

        sig_errno = 0;
        check("nor to ask about", sig_set_has(&set, 65) == -1);
        check("and says why", sig_errno == EINVAL);

        sig_errno = 0;
        check("a negative number is not a signal",
              sig_set_add(&set, -1) == -1);
        check("and says why", sig_errno == EINVAL);
}

/*
        A handler that runs, and -- the part that is not free -- returns.

        Returning from a signal handler is a system call the program never
        wrote: the kernel put a return address on the frame and something has
        to be at it that asks for the interrupted registers back. If that
        address is wrong the process dies here rather than reaching the line
        below, so every check after the raise is also a check that the return
        path exists.
*/
static void sig_case_handler_returns(void)
{
        sig_case_reset();

        check("installing a handler",
              sig_handle(SIGUSR1, sig_note_first) != SIG_ERR);
        check("raising it", sig_raise(SIGUSR1) == 0);
        check("the handler ran", sig_first_ran == SIGUSR1);
        check("and the program is still here", 1);

        sig_case_reset();
}

/*
        The disposition round trip, which is what signal returns.

        Three installs and three answers: the default first, then the handler
        that replaced it, then SIG_IGN that replaced that. A libc that returns
        the NEW disposition, or that returns zero always, passes a test that
        only checks the handler ran.
*/
static void sig_case_disposition(void)
{
        sig_handler previous;

        sig_case_reset();

        previous = sig_handle(SIGUSR1, sig_note_first);
        check("the first install reports the default", previous == SIG_DFL);

        previous = sig_handle(SIGUSR1, SIG_IGN);
        check("the second reports the handler", previous == sig_note_first);

        sig_ignored_ran = 0;
        check("raising an ignored signal succeeds", sig_raise(SIGUSR1) == 0);
        check("and nothing ran", sig_first_ran == 0);

        previous = sig_handle(SIGUSR1, SIG_DFL);
        check("and the third reports SIG_IGN", previous == SIG_IGN);

        sig_case_reset();
}

/*
        sigaction, read back field by field.

        The mask is the field this is really about. It is also the field that
        a single four-word structure gets wrong on exactly one of the three
        machines, because riscv64's kernel struct sigaction has no restorer
        slot and its mask therefore sits eight bytes earlier than on x86_64
        and arm64. A read back that finds the mask where the caller put it is
        the cheapest proof that both layouts are right.
*/
static void sig_case_action_round_trip(void)
{
        sig_action wanted;
        sig_action previous;

        sig_case_reset();

        sig_zero(&wanted, sizeof wanted);
        sig_zero(&previous, sizeof previous);

        sig_handler_of(wanted) = sig_note_first;
        sig_flags_of(wanted) = SA_RESTART;
        sig_set_empty(&sig_mask_of(wanted));
        sig_set_add(&sig_mask_of(wanted), SIGUSR2);
        sig_set_add(&sig_mask_of(wanted), SIGALRM);

        check("installing a full disposition",
              sig_action_change(SIGUSR1, &wanted, 0) == 0);
        check("reading it back",
              sig_action_change(SIGUSR1, 0, &previous) == 0);

        check("the handler came back",
              sig_handler_of(previous) == sig_note_first);
        check("the flags a caller set came back",
              sig_flags_asked(previous) == SA_RESTART);
        check("the mask came back",
              sig_set_has(&sig_mask_of(previous), SIGUSR2) == 1);
        check("and the rest of the mask came back",
              sig_set_has(&sig_mask_of(previous), SIGALRM) == 1);
        check("and nothing else got into it",
              sig_set_has(&sig_mask_of(previous), SIGINT) == 0);

        check("null for both is a legal enquiry about nothing",
              sig_action_change(SIGUSR1, 0, 0) == 0);

        sig_case_reset();
}

/*
        The mask a disposition carries, which is the case this whole family
        was written around.

        SIGUSR1's handler is installed with SIGUSR2 in its mask, and the
        handler raises SIGUSR2 and then looks. If the mask arrived, the second
        handler cannot have run: the kernel is holding SIGUSR2 back until the
        first handler returns. If the mask did not arrive -- if it was written
        into a restorer slot the kernel does not read as a mask -- the second
        handler runs immediately, nested inside the first, and the flag it
        sets is already there when the first looks.

        Both outcomes leave a program that works. Only one of them is what the
        caller asked for, and the difference is invisible to any test that
        checks only that handlers run.
*/
static void sig_case_action_mask(void)
{
        sig_action wanted;

        sig_case_reset();

        sig_zero(&wanted, sizeof wanted);
        sig_handler_of(wanted) = sig_note_second;
        sig_flags_of(wanted) = SA_RESTART;
        sig_set_empty(&sig_mask_of(wanted));
        check("the second signal gets a handler",
              sig_action_change(SIGUSR2, &wanted, 0) == 0);

        sig_zero(&wanted, sizeof wanted);
        sig_handler_of(wanted) = sig_note_first_and_raise;
        sig_flags_of(wanted) = SA_RESTART;
        sig_set_empty(&sig_mask_of(wanted));
        sig_set_add(&sig_mask_of(wanted), SIGUSR2);
        check("the first gets one that blocks the second",
              sig_action_change(SIGUSR1, &wanted, 0) == 0);

        check("raising the first", sig_raise(SIGUSR1) == 0);

        check("the first handler ran", sig_first_ran == SIGUSR1);
        check("the second was held back while it did",
              sig_second_seen_inside_first == 0);
        check("and arrived once the first returned",
              sig_second_ran == SIGUSR2);

        sig_case_reset();
}

/*
        A blocked signal, which does not go away and does not arrive.

        Three separate claims and each one is a way for a mask to be wrong:
        the handler has not run, sigpending says the signal is waiting, and
        unblocking delivers it before sigprocmask returns rather than at some
        later point.
*/
static void sig_case_blocked_stays_pending(void)
{
        sig_set block;
        sig_set pending;

        sig_case_reset();

        check("a handler for the signal about to be blocked",
              sig_handle(SIGUSR2, sig_note_second) != SIG_ERR);

        sig_set_empty(&block);
        sig_set_add(&block, SIGUSR2);

        check("blocking it", sig_mask_change(SIG_BLOCK, &block, 0) == 0);
        check("raising it while blocked", sig_raise(SIGUSR2) == 0);
        check("nothing ran", sig_second_ran == 0);

        sig_set_empty(&pending);
        check("asking what is pending", sig_mask_pending(&pending) == 0);
        check("the blocked signal is pending",
              sig_set_has(&pending, SIGUSR2) == 1);
        check("and nothing else is", sig_set_has(&pending, SIGUSR1) == 0);

        check("unblocking it", sig_mask_change(SIG_UNBLOCK, &block, 0) == 0);
        check("delivered it", sig_second_ran == SIGUSR2);

        sig_set_empty(&pending);
        check("asking again", sig_mask_pending(&pending) == 0);
        check("and nothing is pending now",
              sig_set_has(&pending, SIGUSR2) == 0);

        sig_case_reset();
}

/*
        The three things how can be, and the one thing it cannot.

        SIG_SETMASK is the one worth watching: it replaces rather than adds,
        so a library that implemented all three as an or would pass the first
        two cases and fail this one.
*/
static void sig_case_mask_change(void)
{
        sig_set one;
        sig_set two;
        sig_set had;
        sig_set none;

        sig_case_reset();

        sig_set_empty(&none);
        sig_set_empty(&one);
        sig_set_add(&one, SIGUSR1);
        sig_set_empty(&two);
        sig_set_add(&two, SIGUSR2);

        check("starting from nothing blocked",
              sig_mask_change(SIG_SETMASK, &none, 0) == 0);

        check("blocking one", sig_mask_change(SIG_BLOCK, &one, &had) == 0);
        check("and being told what was blocked before",
              sig_set_has(&had, SIGUSR1) == 0);

        check("blocking a second", sig_mask_change(SIG_BLOCK, &two, &had) == 0);
        check("the first is still blocked", sig_set_has(&had, SIGUSR1) == 1);

        check("reading the mask without changing it",
              sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("both are blocked",
              sig_set_has(&had, SIGUSR1) == 1 &&
                      sig_set_has(&had, SIGUSR2) == 1);

        check("setting replaces rather than adds",
              sig_mask_change(SIG_SETMASK, &two, &had) == 0);
        check("reading it back", sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("only the one set is blocked",
              sig_set_has(&had, SIGUSR2) == 1 &&
                      sig_set_has(&had, SIGUSR1) == 0);

        check("unblocking", sig_mask_change(SIG_UNBLOCK, &two, 0) == 0);
        check("reading it back", sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("nothing is blocked", sig_set_has(&had, SIGUSR2) == 0);

        sig_errno = 0;
        check("a how that is not one of the three fails",
              sig_mask_change(77, &one, 0) == -1);
        check("and says why", sig_errno == EINVAL);
        check("and changed nothing",
              sig_mask_change(SIG_BLOCK, 0, &had) == 0 &&
                      sig_set_has(&had, SIGUSR1) == 0);

        sig_case_reset();
}

/*
        sigsuspend, whose success is spelled -1 and EINTR.

        The signal is blocked, raised so that it is pending, and then the mask
        handed to sigsuspend leaves it out -- so the pending signal is
        delivered the moment the new mask takes effect, the handler runs, and
        sigsuspend comes back having restored the old mask. The last check is
        that restoration: the signal is blocked again afterwards, because it
        was blocked before.
*/
static void sig_case_suspend(void)
{
        sig_set block;
        sig_set open;
        sig_set had;

        sig_case_reset();

        check("a handler", sig_handle(SIGUSR1, sig_note_suspend) != SIG_ERR);

        sig_set_empty(&block);
        sig_set_add(&block, SIGUSR1);
        sig_set_empty(&open);

        check("blocking it", sig_mask_change(SIG_SETMASK, &block, 0) == 0);
        check("raising it while blocked", sig_raise(SIGUSR1) == 0);
        check("nothing has run", sig_suspend_ran == 0);

        sig_errno = 0;
        check("suspending with it unblocked returns",
              sig_mask_suspend(&open) == -1);
        check("interrupted, which is the only way out", sig_errno == EINTR);
        check("and the handler ran", sig_suspend_ran == SIGUSR1);

        check("reading the mask back", sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("suspend put the old mask back",
              sig_set_has(&had, SIGUSR1) == 1);

        sig_set_empty(&open);
        check("clearing up", sig_mask_change(SIG_SETMASK, &open, 0) == 0);

        sig_case_reset();
}

/*
        alarm, which is a timer on two of the three machines and a system call
        on the other, and pause, which is a system call on one and a suspend
        on the other two.

        A whole second of real waiting, and it is not skippable: proving that
        SIGALRM arrives means letting it arrive. pause is what waits, and its
        return is the second claim -- it too answers -1 and EINTR and never
        anything else.

        The remaining-seconds answer is checked by arming for two seconds and
        immediately cancelling. glibc rounds the leftover up to whole seconds,
        so an alarm set a microsecond ago has two seconds left rather than
        one, and rounding the other way would report zero for an alarm that
        was certainly armed.
*/
static void sig_case_alarm_and_pause(void)
{
        unsigned int left;

        sig_case_reset();

        check("cancelling an alarm nobody set", sig_alarm(0) == 0);

        left = sig_alarm(2);
        check("arming one reports no previous", left == 0);

        left = sig_alarm(0);
        check("cancelling it reports what was left, rounded up", left == 2);

        check("a handler for it",
              sig_handle(SIGALRM, sig_note_alarm) != SIG_ERR);

        left = sig_alarm(1);
        check("arming again", left == 0);

        sig_errno = 0;
        check("pause waits and then reports being interrupted",
              sig_wait() == -1);
        check("and says why", sig_errno == EINTR);
        check("the alarm arrived", sig_alarm_ran == SIGALRM);

        sig_case_reset();
}

/*
        The mask across a mark and a jump, which is the whole reason
        sigsetjmp is a different routine from setjmp.

        Twice, because the second argument is what decides it. With a one, the
        mask in force at the mark is written down and put back by the jump, so
        the unblocking done in between is undone. With a zero, nothing is
        written down and the unblocking survives -- which is setjmp's own
        behaviour and is what this library's setjmp has always done.

        A library that saved the mask unconditionally passes the first half
        and fails the second; one that ignored the argument passes the second
        and fails the first. Neither half alone says anything.
*/
static void sig_case_jump_mask(void)
{
        sig_set one;
        sig_set had;
        volatile int arrivals;

        sig_case_reset();

        sig_set_empty(&one);
        sig_set_add(&one, SIGUSR1);
        sig_set_empty(&had);

        check("blocking a signal before the mark",
              sig_mask_change(SIG_SETMASK, &one, 0) == 0);

        arrivals = 0;

        if (sig_jump_mark(sig_place, 1) == 0)
        {
                arrivals = 1;
                sig_mask_change(SIG_UNBLOCK, &one, 0);
                sig_jump_to_mark(sig_place, 3);
                check("the jump did not return", 0);
        }

        check("the mark was reached twice", arrivals == 1);
        check("reading the mask after the jump",
              sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("asking for the mask put it back",
              sig_set_has(&had, SIGUSR1) == 1);

        check("blocking again", sig_mask_change(SIG_SETMASK, &one, 0) == 0);

        arrivals = 0;

        if (sig_jump_mark(sig_place, 0) == 0)
        {
                arrivals = 1;
                sig_mask_change(SIG_UNBLOCK, &one, 0);
                sig_jump_to_mark(sig_place, 4);
                check("the jump did not return", 0);
        }

        check("the mark was reached twice", arrivals == 1);
        check("reading the mask after the jump",
              sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("not asking left it alone", sig_set_has(&had, SIGUSR1) == 0);

        sig_case_reset();
}

//      The value the jump carries, and the one value it may not carry.
static void sig_case_jump_value(void)
{
        volatile int arrivals;
        int landed;

        sig_case_reset();

        arrivals = 0;
        landed = sig_jump_mark(sig_place, 1);

        if (landed == 0)
        {
                arrivals = 1;
                sig_jump_to_mark(sig_place, 42);
        }

        check("the mark answered zero the first time", arrivals == 1);
        check("and the jump's own value the second", landed == 42);

        arrivals = 0;
        landed = sig_jump_mark(sig_place, 0);

        if (landed == 0)
        {
                arrivals = 1;
                sig_jump_to_mark(sig_place, 0);
        }

        check("the mark answered zero the first time", arrivals == 1);
        check("a jump asking for zero arrives as one", landed == 1);

        sig_case_reset();
}

/*
        Leaving a handler by jumping out of it, which is the shape every
        program that recovers from SIGSEGV or times out a read is written in.

        The kernel blocks the signal being delivered for as long as its
        handler runs, and a handler that jumps out instead of returning never
        reaches the return that would unblock it. So a plain longjmp out of a
        SIGUSR1 handler leaves SIGUSR1 blocked for the rest of the process,
        and the second SIGUSR1 never arrives -- silently, once, much later.

        That is the bug sigsetjmp exists to not have, and the last two checks
        are the whole of it: after jumping out, the signal is not blocked, and
        raising it again works.
*/
static void sig_case_jump_from_handler(void)
{
        sig_set had;
        volatile int arrivals;
        int landed;

        sig_case_reset();

        check("a handler that leaves rather than returns",
              sig_handle(SIGUSR1, sig_leave_from_handler) != SIG_ERR);

        arrivals = 0;
        landed = sig_jump_mark(sig_place, 1);

        if (landed == 0)
        {
                arrivals = 1;
                sig_raise(SIGUSR1);
                check("the handler did not come back", 0);
        }

        check("the mark was reached twice", arrivals == 1);
        check("with the value the handler jumped with", landed == 1);
        check("and the handler had run", sig_first_ran == SIGUSR1);

        check("reading the mask", sig_mask_change(SIG_BLOCK, 0, &had) == 0);
        check("the signal is not still blocked",
              sig_set_has(&had, SIGUSR1) == 0);

        sig_first_ran = 0;
        check("a handler that returns this time",
              sig_handle(SIGUSR1, sig_note_first) != SIG_ERR);
        check("raising it again", sig_raise(SIGUSR1) == 0);
        check("and it arrives", sig_first_ran == SIGUSR1);

        sig_case_reset();
}

static void sig_cases_all(void)
{
        sig_case_set_operations();
        sig_case_handler_returns();
        sig_case_disposition();
        sig_case_action_round_trip();
        sig_case_action_mask();
        sig_case_blocked_stays_pending();
        sig_case_mask_change();
        sig_case_suspend();
        sig_case_alarm_and_pause();
        sig_case_jump_mask();
        sig_case_jump_value();
        sig_case_jump_from_handler();
}

#endif // STANDARD_MODERN_C_TEST_SIGNAL_CASES
