/*
        Experimental C standard library

        leaving: what a program writes on its way out, what it does not, and
        what a forked child must never write a second time

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/
#include "../compiler_memory.c"

/*
        Nothing here can be answered from inside the process asking it.

        Every question in this file is about what reached a descriptor at the
        moment a process stopped existing, and a process cannot report on its
        own last act. So each one is asked of a child: the test clones, the
        child writes something and leaves by the path under examination, and
        the parent reads the pipe the child's standard output was and counts
        the bytes that actually arrived.

        The child's standard output is set fully buffered on purpose. Whether
        a stream is line buffered is decided by asking the descriptor if it is
        a terminal, and every question here is about bytes that are still in a
        buffer when the process ends -- run from a terminal without this, the
        buffer would already be empty and every check would pass while proving
        nothing.

        One thing is deliberately not asked of a child, because it cannot be:
        whether returning from main flushes. main belongs to the umbrella's
        startup shim, a child cannot return from it without unwinding the
        whole test, and there is nowhere else to stand. So this file proves it
        by being it. The verdict line at the bottom is written into the log
        buffer and never flushed by hand; the only thing that can put it in
        front of the runner's grep is the shim calling exit on the way out of
        main. If that stops working this lane does not report a failure, it
        reports no verdict at all, which the runner treats as a failure and
        says so in those words.
*/

//      A literal where a string_address is wanted.
#define text(literal) ((string_address)(literal))

#define test(test_name) bool test_##test_name(void)
#define case(test_name)                        \
        {                                      \
                (string_address)#test_name,    \
                test_##test_name               \
        }
#define fail(condition)   \
        if (!(condition)) \
        return false

typedef bool(address_to test_function)(void);

typedef struct
{
        string_address name;
        test_function function;
} test_case;

/*
        The bytes a child writes, and never more than one buffer's worth of
        them, so that nothing here depends on how large BUFSIZ happens to be.
*/
#define LEAVING_FIRST "first\n"
#define LEAVING_SECOND "second\n"
#define LEAVING_HANDLER "handler\n"

//      What a cloned child is being asked to do, and the status it leaves
//      with, which is checked beside the bytes so that a child that died on
//      the way to the interesting part cannot be mistaken for one that got
//      there and wrote nothing.
#define LEAVE_STREAM_EXIT 21
#define LEAVE_STREAM_UNDERSCORE 22
#define LEAVE_STREAM_CAPITAL 23
#define LEAVE_STREAM_QUICK 24
#define LEAVE_LOG_EXIT 25
#define LEAVE_LOG_BY_HAND 26
#define LEAVE_BOTH 27
#define LEAVE_FORK_STREAM 28
#define LEAVE_FORK_LOG 29
#define LEAVE_FORK_TWICE 30
#define LEAVE_FORK_FLUSHED 31
#define LEAVE_FORK_CHILD_WROTE 32
#define LEAVE_ATEXIT 33
#define LEAVE_FORK_CHILD_FLUSHED 35

static fn leaving_handler(void)
{
        printf("%s", LEAVING_HANDLER);
}

/*
        Fork, let the child leave by exit, and wait for it.

        This is the shape the shell uses at every one of its twenty exit sites
        and the shape a failed execve leaves behind: a child that has nothing
        of its own to say, inherits whatever the parent had not written yet,
        and stops. It is the one place a flush at exit could print the
        parent's output twice.
*/
static fn leaving_fork_and_leave(void)
{
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
        positive raw = 0;

        if (child == 0)
                exit(0);

        if (child > 0)
                system_wait4_retry(child, address_of raw, 0, null);
}

DEAD_END static fn leaving_child(b32 which)
{
        bipolar grandchild;
        positive raw = 0;

        stream_set_buffering(stdout, null, _IOFBF, BUFSIZ);

        switch (which)
        {
        case LEAVE_STREAM_EXIT:
                printf("%s", LEAVING_FIRST);
                exit(LEAVE_STREAM_EXIT);
                break;

        case LEAVE_STREAM_UNDERSCORE:
                printf("%s", LEAVING_FIRST);
                _exit(LEAVE_STREAM_UNDERSCORE);
                break;

        case LEAVE_STREAM_CAPITAL:
                printf("%s", LEAVING_FIRST);
                _Exit(LEAVE_STREAM_CAPITAL);
                break;

        case LEAVE_STREAM_QUICK:
                printf("%s", LEAVING_FIRST);
                quick_exit(LEAVE_STREAM_QUICK);
                break;

        case LEAVE_LOG_EXIT:
                log(str(LEAVING_FIRST));
                exit(LEAVE_LOG_EXIT);
                break;

        case LEAVE_LOG_BY_HAND:
                log(str(LEAVING_FIRST));
                log_flush();
                exit(LEAVE_LOG_BY_HAND);
                break;

        case LEAVE_BOTH:
                printf("%s", LEAVING_FIRST);
                log(str(LEAVING_SECOND));
                log_flush();
                exit(LEAVE_BOTH);
                break;

        case LEAVE_FORK_STREAM:
                printf("%s", LEAVING_FIRST);
                leaving_fork_and_leave();
                exit(LEAVE_FORK_STREAM);
                break;

        case LEAVE_FORK_LOG:
                log(str(LEAVING_FIRST));
                leaving_fork_and_leave();
                log_flush();
                exit(LEAVE_FORK_LOG);
                break;

        case LEAVE_FORK_TWICE:
                printf("%s", LEAVING_FIRST);
                leaving_fork_and_leave();
                leaving_fork_and_leave();
                exit(LEAVE_FORK_TWICE);
                break;

        case LEAVE_FORK_FLUSHED:
                printf("%s", LEAVING_FIRST);
                fflush(stdout);

                grandchild = system_call_2(syscall(clone), SIGCHLD, 0);

                if (grandchild == 0)
                {
                        printf("%s", LEAVING_SECOND);
                        exit(0);
                }

                if (grandchild > 0)
                        system_wait4_retry(grandchild, address_of raw, 0, null);

                exit(LEAVE_FORK_FLUSHED);
                break;

        case LEAVE_FORK_CHILD_WROTE:
                printf("%s", LEAVING_FIRST);

                grandchild = system_call_2(syscall(clone), SIGCHLD, 0);

                if (grandchild == 0)
                {
                        printf("%s", LEAVING_SECOND);
                        exit(0);
                }

                if (grandchild > 0)
                        system_wait4_retry(grandchild, address_of raw, 0, null);

                exit(LEAVE_FORK_CHILD_WROTE);
                break;

        case LEAVE_FORK_CHILD_FLUSHED:
                printf("%s", LEAVING_FIRST);

                grandchild = system_call_2(syscall(clone), SIGCHLD, 0);

                if (grandchild == 0)
                {
                        fflush(stdout);
                        _exit(0);
                }

                if (grandchild > 0)
                        system_wait4_retry(grandchild, address_of raw, 0, null);

                exit(LEAVE_FORK_CHILD_FLUSHED);
                break;

        case LEAVE_ATEXIT:
                atexit(leaving_handler);
                printf("%s", LEAVING_FIRST);
                exit(LEAVE_ATEXIT);
                break;

        default:
                break;
        }

        //      Only reached when the path above did not leave, which is the
        //      failure this reports.
        _Exit(70);
}

/*
        Run one of those and bring back both halves of the answer: the status
        a shell would have reported, and the exact bytes that reached the
        pipe the child's standard output was.

        The parent flushes its own buffers before cloning, which is the advice
        POSIX has always given and which this file follows so that what the
        child inherits is only ever what the scenario put there on purpose.
*/
static b32 leaving_run(b32 which, p8 address_to seen, positive room,
                       positive address_to filled)
{
        b32 ends[2];
        bipolar child;
        positive raw = 0;

        *filled = 0;
        seen[0] = end;

        if (system_call_2(syscall(pipe2), (positive)(address_any)ends, 0) < 0)
                return -1;

        fflush(stdout);
        log_flush();

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child < 0)
                return -1;

        if (child == 0)
        {
                system_call_1(syscall(close), (positive)ends[0]);
                system_call_3(syscall(dup3), (positive)ends[1], 1, 0);
                system_call_1(syscall(close), (positive)ends[1]);
                leaving_child(which);
        }

        system_call_1(syscall(close), (positive)ends[1]);

        for (;;)
        {
                bipolar got = system_read_retry((positive)ends[0], seen + *filled,
                                                room - 1 - *filled);

                if (got <= 0)
                        break;

                *filled += (positive)got;

                if (*filled >= room - 1)
                        break;
        }

        seen[*filled] = end;

        system_call_1(syscall(close), (positive)ends[0]);

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
                return -1;

        return wait_status_code(raw);
}

//      Every check in the first half is the same two questions asked of a
//      different way of leaving, so they are asked once, here.
static bool leaving_says(b32 which, string_address expected)
{
        p8 seen[256];
        positive filled = 0;

        if (leaving_run(which, seen, sizeof(seen), address_of filled) != which)
                return false;

        if (filled != string_length(expected))
                return false;

        return string_compare((string_address)seen, expected) == 0;
}

/*
        The divergence this family was written to close.

        printf then exit lost its bytes: the C spelling of exit was a macro
        nothing turned on, so the call reached the assembly trap and the
        buffer went with the address space. Every C program on earth assumes
        otherwise and glibc has always done otherwise.
*/
test(printf_then_exit_prints)
{
        return leaving_says(LEAVE_STREAM_EXIT, text(LEAVING_FIRST));
}

//      The three that must not flush, which is the whole of what distinguishes
//      them from the one above.
test(printf_then_underscore_exit_prints_nothing)
{
        return leaving_says(LEAVE_STREAM_UNDERSCORE, text(""));
}

test(printf_then_capital_exit_prints_nothing)
{
        return leaving_says(LEAVE_STREAM_CAPITAL, text(""));
}

test(printf_then_quick_exit_prints_nothing)
{
        return leaving_says(LEAVE_STREAM_QUICK, text(""));
}

test(an_atexit_handler_writes_before_the_flush)
{
        return leaving_says(LEAVE_ATEXIT,
                            text(LEAVING_FIRST LEAVING_HANDLER));
}

/*
        The hazard, and it is the reason this was not simply switched on.

        The child inherits a buffer holding bytes the parent wrote and has not
        written out. A flush at exit that did not ask whose bytes those were
        would print them from the child and then again from the parent, which
        is precisely what glibc does and what the folklore answer -- flush
        before you fork -- is folklore about.
*/
test(a_forked_child_leaving_does_not_print_the_parents_bytes_again)
{
        return leaving_says(LEAVE_FORK_STREAM, text(LEAVING_FIRST));
}

//      Twice over, because a mechanism that only survives one generation is
//      not a mechanism.
test(two_forked_children_leaving_still_print_it_once)
{
        return leaving_says(LEAVE_FORK_TWICE, text(LEAVING_FIRST));
}

/*
        And the case the ownership stamp exists for: a parent that did flush
        before it forked hands its child an empty buffer, so the child's own
        output is its own and goes out at exit exactly as an unforked
        program's would. A guard that asked only "am I a forked child" would
        throw this away.
*/
test(a_forked_child_with_a_clean_buffer_prints_its_own)
{
        return leaving_says(LEAVE_FORK_FLUSHED,
                            text(LEAVING_FIRST LEAVING_SECOND));
}

/*
        The residue, pinned rather than hidden.

        A child that writes into a buffer that already held its parent's loses
        its own bytes with the inherited ones, because the two are in one
        buffer with nothing between them to say where one stops. glibc writes
        both and then writes the parent's a second time from the parent. This
        writes the parent's once and drops the child's, which is a different
        wrong answer to a question POSIX answers by telling the caller to
        flush before forking. The check is here so a future change that alters
        it has to say so.
*/
test(a_forked_child_writing_into_an_inherited_buffer_loses_its_own)
{
        return leaving_says(LEAVE_FORK_CHILD_WROTE, text(LEAVING_FIRST));
}

/*
        The guard is on the implicit flush only. A child that calls fflush
        itself is asking for exactly what POSIX says fflush does, gets it, and
        gets the double print with it -- the same as glibc, and the reason the
        advice is to flush before the fork rather than after it.
*/
test(an_explicit_flush_in_a_child_still_writes_what_it_inherited)
{
        return leaving_says(LEAVE_FORK_CHILD_FLUSHED,
                            text(LEAVING_FIRST LEAVING_FIRST));
}

/*
        The log buffer is coarser, and the block above stdlib_buffers_are_ours
        says why: it lives in assembly, library.c holds assembly and
        declarations and nothing else, so there is nowhere in it to keep the
        stamp a stream keeps. What gates it instead is the identity recorded
        at startup, so a forked child's implicit flush leaves the log alone.

        That is not a loss against anything that ever worked -- no exit path
        in this library flushed the log before this family wrote one -- and
        the explicit call still does what it always did, which is what the
        shell in this tree relies on at every site where it leaves a child.
*/
test(a_forked_childs_log_buffer_is_not_flushed_for_it)
{
        return leaving_says(LEAVE_LOG_EXIT, text(""));
}

test(a_forked_child_can_still_flush_the_log_itself)
{
        return leaving_says(LEAVE_LOG_BY_HAND, text(LEAVING_FIRST));
}

test(a_forked_childs_log_flush_is_not_doubled_by_a_fork)
{
        return leaving_says(LEAVE_FORK_LOG, text(LEAVING_FIRST));
}

/*
        The two buffers are two buffers, and a program that writes through
        both has to be able to predict which arrives first.

        They are independent all the way down: the stream buffer is stream.c's
        and the log buffer is any.inc's, they are flushed by different code and
        they reach descriptor one by different calls. So a hand flush of the
        log goes out immediately and a printf still waiting in the stream
        buffer goes out later, whatever order the two calls were written in.
        This pins that, because a program mixing printf and
        string_format(log, ...) and expecting source order is going to be
        surprised, and the surprise should be documented rather than found.
*/
test(a_hand_flushed_log_lands_before_a_stream_still_waiting)
{
        return leaving_says(LEAVE_BOTH, text(LEAVING_SECOND LEAVING_FIRST));
}

/*
        environ, which the stdlib family left out until there was a C startup
        to point it at anything.

        The first check has to be first: getenv takes a private copy of the
        vector the moment it is called, and publishes the copy over what the
        shim published, so this is the only place in the file where the
        kernel's own vector is still what environ holds.
*/
static string_address address_to leaving_environ_at_start = null;

test(environ_holds_the_kernel_vector_before_anything_asks)
{
        fail(!is_null(leaving_environ_at_start));
        fail(leaving_environ_at_start == program_environment_list());
        fail(!is_null(leaving_environ_at_start[0]));

        return true;
}

//      Every entry, as a name and a value, has to be the entry getenv finds
//      under that name. A vector that were merely non-null would pass a
//      liveness check and still be the wrong vector.
test(environ_agrees_with_getenv_entry_by_entry)
{
        string_address address_to walk;
        positive seen = 0;

        for (walk = environ; !is_null(*walk); walk++)
        {
                p8 name[256];
                p8 address_to equals = string_first_of(*walk, '=');
                positive length;
                string_address found;

                fail(!is_null(equals));

                length = (positive)(equals - *walk);

                fail(length > 0 && length < sizeof(name));

                memory_copy(name, *walk, length);
                name[length] = end;

                found = getenv((string_address)name);

                fail(!is_null(found));
                fail(string_compare(found, (string_address)(equals + 1)) == 0);

                seen++;
        }

        fail(seen > 0);

        return true;
}

//      getenv is what takes the private copy, so by here environ must be the
//      copy rather than the kernel's vector it started as.
test(environ_is_the_vector_the_family_owns_once_it_owns_one)
{
        fail(!is_null(getenv(text("PATH"))) || true);
        fail(environ == stdlib_environment_list());

        return true;
}

/*
        Growth is where a published pointer goes stale, and it is the reason
        publishing is not one act at startup. The vector starts with eight
        spare slots and is replaced wholesale when they run out, so forty
        names guarantee at least one replacement and the check is that environ
        followed it.
*/
#define LEAVING_NAMES 40
#define LEAVING_MADE "LEAVING_MADE_"
#define LEAVING_VALUE "value "

//      Built with the library rather than with snprintf: positive_into is the
//      assembly that turns a number into digits and answers how many it wrote,
//      and the two prefixes are literals whose length the known-size copy
//      folds where the call is written.
static fn leaving_name(p8 address_to name, p8 address_to value, positive index)
{
        positive length;

        memory_copy(name, LEAVING_MADE, sizeof(LEAVING_MADE) - 1);
        length = positive_into(name + sizeof(LEAVING_MADE) - 1, index);
        name[sizeof(LEAVING_MADE) - 1 + length] = end;

        memory_copy(value, LEAVING_VALUE, sizeof(LEAVING_VALUE) - 1);
        length = positive_into(value + sizeof(LEAVING_VALUE) - 1, index);
        value[sizeof(LEAVING_VALUE) - 1 + length] = end;
}

test(environ_follows_setenv_through_a_growth)
{
        positive index;

        for (index = 0; index < LEAVING_NAMES; index++)
        {
                p8 name[32];
                p8 value[32];

                leaving_name(name, value, index);

                fail(setenv((string_address)name, (string_address)value, 1) == 0);
        }

        fail(environ == stdlib_environment_list());

        for (index = 0; index < LEAVING_NAMES; index++)
        {
                p8 name[32];
                p8 value[32];
                string_address address_to walk;
                bool found = false;

                leaving_name(name, value, index);

                //      Walked rather than looked up, because the question is
                //      whether the vector environ names holds the entry, not
                //      whether getenv can find it in the one it holds itself.
                for (walk = environ; !is_null(*walk); walk++)
                {
                        p8 address_to equals = string_first_of(*walk, '=');

                        if (is_null(equals))
                                continue;

                        if (string_compare((string_address)(equals + 1),
                                           (string_address)value) != 0)
                                continue;

                        if ((positive)(equals - *walk) !=
                            string_length((string_address)name))
                                continue;

                        if (memory_compare(*walk, name,
                                           string_length((string_address)name)) == 0)
                                found = true;
                }

                fail(found);
        }

        return true;
}

//      unsetenv moves entries about inside the vector it was given, so the
//      pointer must not move and the name must be gone from what environ
//      names rather than only from what getenv looks at.
test(environ_follows_unsetenv_without_moving)
{
        string_address address_to before = environ;
        string_address address_to walk;
        positive left = 0;

        fail(setenv(text("LEAVING_GOING"), text("here"), 1) == 0);
        fail(!is_null(getenv(text("LEAVING_GOING"))));
        fail(unsetenv(text("LEAVING_GOING")) == 0);
        fail(is_null(getenv(text("LEAVING_GOING"))));
        fail(environ == before);

        for (walk = environ; !is_null(*walk); walk++)
        {
                p8 address_to equals = string_first_of(*walk, '=');

                if (is_null(equals))
                        continue;

                if ((positive)(equals - *walk) == 13 &&
                    memory_compare(*walk, "LEAVING_GOING", 13) == 0)
                        left++;
        }

        fail(left == 0);

        return true;
}

//      putenv installs the caller's own bytes into the same vector, and
//      environ has to be showing that vector.
static p8 leaving_entry[32] = "LEAVING_HELD=before";

test(environ_shows_what_putenv_installed)
{
        string_address address_to walk;
        bool found = false;

        fail(putenv(leaving_entry) == 0);

        for (walk = environ; !is_null(*walk); walk++)
                if (*walk == leaving_entry)
                        found = true;

        fail(found);

        return true;
}

static test_case test_cases[] = {
        case(environ_holds_the_kernel_vector_before_anything_asks),
        case(printf_then_exit_prints),
        case(printf_then_underscore_exit_prints_nothing),
        case(printf_then_capital_exit_prints_nothing),
        case(printf_then_quick_exit_prints_nothing),
        case(an_atexit_handler_writes_before_the_flush),
        case(a_forked_child_leaving_does_not_print_the_parents_bytes_again),
        case(two_forked_children_leaving_still_print_it_once),
        case(a_forked_child_with_a_clean_buffer_prints_its_own),
        case(a_forked_child_writing_into_an_inherited_buffer_loses_its_own),
        case(an_explicit_flush_in_a_child_still_writes_what_it_inherited),
        case(a_forked_childs_log_buffer_is_not_flushed_for_it),
        case(a_forked_child_can_still_flush_the_log_itself),
        case(a_forked_childs_log_flush_is_not_doubled_by_a_fork),
        case(a_hand_flushed_log_lands_before_a_stream_still_waiting),
        case(environ_agrees_with_getenv_entry_by_entry),
        case(environ_is_the_vector_the_family_owns_once_it_owns_one),
        case(environ_follows_setenv_through_a_growth),
        case(environ_follows_unsetenv_without_moving),
        case(environ_shows_what_putenv_installed),
        {null, null},
};

b32 main(void)
{
        test_case address_to walk = test_cases;
        positive passed = 0;
        positive failed = 0;

        //      Read before anything else in the program can call getenv,
        //      because the first getenv replaces what the shim published.
        leaving_environ_at_start = environ;

        log_direct(str("leaving tests\n\n"));

        while (walk->name)
        {
                bool result;

                log_direct(walk->name, string_length(walk->name));

                result = walk->function();

                if (result)
                {
                        log_direct(str(" PASSED\n"));
                        passed++;
                }
                else
                {
                        log_direct(str(" ----- FAILED\n"));
                        failed++;
                }

                walk++;
        }

        /*
                Written into the log buffer and deliberately not flushed.

                Returning from main is defined by C to be a call to exit, and
                the umbrella's startup shim is what makes that true here. If
                it stops being true these bytes never leave the buffer, the
                runner finds no verdict line at all, and it says so -- which
                is a louder failure than a count being one short and is the
                only way this particular claim can be checked from inside the
                process making it.
        */
        string_format(log, "\n%p checks, %p failures\n", passed + failed, failed);

        return failed > 0 ? 1 : 0;
}
