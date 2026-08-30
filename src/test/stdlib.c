/*
        Experimental C standard library

        stdlib: the environment, sorting, leaving, and a generator that has to
        agree with glibc's

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/
#include "../compiler_memory.c"

/*
        Half of this family cannot be checked from inside the process it is
        running in. exit runs handlers and then does not come back; abort is
        supposed to kill whatever installed a handler over it; _Exit is
        defined by what it does not do. Every one of those is a statement
        about a process, so the checks that make them are made about a child:
        the test clones, the child leaves by the path being examined, and the
        parent reads the status the kernel gives it back and, where the
        question is about buffered output, the bytes that came out of a pipe.

        The numbers the generator is checked against are glibc's, taken from a
        program linked against it on the same machine and pasted here. They
        are not a plausible-looking sequence; they are the sequence, and a
        change in this file that alters one of them has changed the answer.
*/

//      A literal where a string_address is wanted. str() in library.c gives a
//      pointer and a length together, which is the wrong shape for a call that
//      takes one argument.
#define text(literal) ((string_address)(literal))

#include "named_cases.inc"

//      What a cloned child is being asked to do. The child never returns from
//      any of these; the number it leaves with is the answer.
#define LEAVE_EXIT 1
#define LEAVE_QUICK 2
#define LEAVE_IMMEDIATE 3
#define LEAVE_ABORT 4
#define LEAVE_ABORT_IGNORED 5
#define LEAVE_ABORT_BLOCKED 6
#define LEAVE_ORDER 7
#define LEAVE_UNNAMED 8

#define TEST_MARKER "buffered\n"
#define TEST_SIGNAL_ABRT 6

static b32 stdlib_test_order = 0;

static fn stdlib_test_mark_one(void) { stdlib_test_order = stdlib_test_order * 10 + 1; }
static fn stdlib_test_mark_two(void) { stdlib_test_order = stdlib_test_order * 10 + 2; }
static fn stdlib_test_mark_three(void) { stdlib_test_order = stdlib_test_order * 10 + 3; }

//      Registered first, so it runs last, and it is the only place where the
//      whole order is known. Three handlers registered one two three and run
//      in reverse spell 321; anything else leaves with one.
static fn stdlib_test_mark_judge(void)
{
        _Exit(stdlib_test_order == 321 ? 0 : 1);
}

//      SIG_IGN is one, and installing it needs no restorer because nothing is
//      ever delivered into this program's own code.
static fn stdlib_test_ignore_abort(void)
{
        positive action[4] = {1, 0, 0, 0};

        system_call_4(syscall(rt_sigaction), TEST_SIGNAL_ABRT,
                      (positive)address_of action, 0, 8);
}

//      SIG_BLOCK is zero.
static fn stdlib_test_block_abort(void)
{
        positive mask = (positive)1 << (TEST_SIGNAL_ABRT - 1);

        system_call_4(syscall(rt_sigprocmask), 0, (positive)address_of mask, 0, 8);
}

DEAD_END static fn stdlib_test_child(b32 which)
{
        //      Left in the buffer on purpose: whether it ever reaches the
        //      pipe is what separates exit from the three that do not flush.
        //
        //      Through a stream rather than through the log, because in a
        //      forked child the two buffers answer differently and this is a
        //      forked child. A stream records which process filled it and this
        //      one filled its own, so exit writes it out; the log buffer lives
        //      in any.inc where there is nowhere to keep that, so an implicit
        //      flush leaves it alone in any child at all. The block above
        //      stdlib_buffers_are_ours says why, and src/test/leaving.c has
        //      both halves under test.
        printf("%s", TEST_MARKER);

        switch (which)
        {
        case LEAVE_EXIT:
                stdlib_exit(5);
                break;

        case LEAVE_QUICK:
                quick_exit(6);
                break;

        case LEAVE_IMMEDIATE:
                _Exit(7);
                break;

        case LEAVE_ABORT:
                abort();
                break;

        case LEAVE_ABORT_IGNORED:
                stdlib_test_ignore_abort();
                abort();
                break;

        case LEAVE_ABORT_BLOCKED:
                stdlib_test_block_abort();
                abort();
                break;

        case LEAVE_ORDER:
                atexit(stdlib_test_mark_judge);
                atexit(stdlib_test_mark_one);
                atexit(stdlib_test_mark_two);
                atexit(stdlib_test_mark_three);
                stdlib_exit(99);
                break;

        default:
                break;
        }

        //      Only reached when the path above was defeated, which is itself
        //      the failure this reports.
        _Exit(70);
}

/*
        Run one of those in a child and bring back both halves of its answer:
        the status the shell would have reported, and how many bytes it wrote
        before it went. The child's standard output is the pipe, so the parent
        is reading exactly what the child flushed and nothing else.

        The parent flushes its own buffer before cloning. A child inherits it,
        and a child that flushed would print the parent's pending output a
        second time -- the hazard the C spelling of exit used to be kept away
        from, and is now guarded against rather than avoided.
*/
static b32 stdlib_test_leave(b32 which, positive address_to wrote)
{
        b32 ends[2];
        bipolar child;
        positive raw = 0;
        p8 seen[64];
        positive filled = 0;

        *wrote = 0;

        if (system_call_2(syscall(pipe2), (positive)(address_any)ends, 0) < 0)
                return -1;

        log_flush();

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child < 0)
                return -1;

        if (child == 0)
        {
                system_call_1(syscall(close), (positive)ends[0]);
                system_call_3(syscall(dup3), (positive)ends[1], 1, 0);
                system_call_1(syscall(close), (positive)ends[1]);
                stdlib_test_child(which);
        }

        system_call_1(syscall(close), (positive)ends[1]);

        for (;;)
        {
                bipolar got = system_read_retry((positive)ends[0], seen + filled,
                                                sizeof(seen) - filled);

                if (got <= 0)
                        break;

                filled += (positive)got;

                if (filled >= sizeof(seen))
                        break;
        }

        system_call_1(syscall(close), (positive)ends[0]);

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
                return -1;

        *wrote = filled;

        return wait_status_code(raw);
}

test(exit_runs_handlers_and_flushes)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_EXIT, address_of wrote) == 5);
        fail(wrote == sizeof(TEST_MARKER) - 1);

        return true;
}

test(exit_runs_handlers_in_reverse)
{
        positive wrote = 0;

        //      Zero only when the three handlers ran three, two, one.
        fail(stdlib_test_leave(LEAVE_ORDER, address_of wrote) == 0);

        return true;
}

test(quick_exit_does_not_flush)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_QUICK, address_of wrote) == 6);
        fail(wrote == 0);

        return true;
}

test(immediate_exit_does_not_flush)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_IMMEDIATE, address_of wrote) == 7);
        fail(wrote == 0);

        return true;
}

test(abort_raises_sigabrt)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_ABORT, address_of wrote) == 128 + TEST_SIGNAL_ABRT);

        return true;
}

//      The two ways a program can try to survive its own abort, and neither
//      of them is allowed to work.
test(abort_beats_an_ignored_signal)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_ABORT_IGNORED, address_of wrote) ==
             128 + TEST_SIGNAL_ABRT);

        return true;
}

test(abort_beats_a_blocked_signal)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_ABORT_BLOCKED, address_of wrote) ==
             128 + TEST_SIGNAL_ABRT);

        return true;
}

/*
        A way of leaving this file does not name reaches the trap.

        This was called returning_from_main_is_the_bare_trap, and the block
        here said that making the C spelling reach a return would mean
        changing assembly inside library.c's graph, which no family owned. It
        did not need to. The umbrella grew a startup shim, the shim calls main
        and then exit, and returning from main now runs the handlers and
        flushes exactly as C says it does; src/test/leaving.c proves that by
        being it, since its verdict line is written into the log buffer and
        never flushed by hand.

        What was always actually under test here is what the name should have
        said: a child handed a number this file has no case for falls off the
        end of the switch, reaches the _Exit below it, and that is the trap --
        status seventy, and nothing written.
*/
test(an_unnamed_way_of_leaving_reaches_the_trap)
{
        positive wrote = 0;

        fail(stdlib_test_leave(LEAVE_UNNAMED, address_of wrote) == 70);
        fail(wrote == 0);

        return true;
}

test(environment_reads_what_the_kernel_gave)
{
        //      PATH is not guaranteed, but something has to be, and a vector
        //      with nothing in it would make every other check here vacuous.
        fail(!is_null(program_environment_list()));
        fail(!is_null(program_environment_list()[0]));
        fail(is_null(getenv(text("A NAME NOTHING WOULD SET"))));

        return true;
}

test(environment_set_then_get)
{
        fail(setenv(text("DAWNING_ONE"), text("first"), 1) == 0);
        fail(string_compare(getenv(text("DAWNING_ONE")), text("first")) == 0);

        //      Without permission to overwrite, the value that was there wins
        //      and the call still succeeds.
        fail(setenv(text("DAWNING_ONE"), text("second"), 0) == 0);
        fail(string_compare(getenv(text("DAWNING_ONE")), text("first")) == 0);

        fail(setenv(text("DAWNING_ONE"), text("second"), 1) == 0);
        fail(string_compare(getenv(text("DAWNING_ONE")), text("second")) == 0);

        //      An empty value is a variable that is set and empty, which is
        //      not the same thing as one that is absent.
        fail(setenv(text("DAWNING_EMPTY"), text(""), 1) == 0);
        fail(!is_null(getenv(text("DAWNING_EMPTY"))));
        fail(getenv(text("DAWNING_EMPTY"))[0] == end);

        return true;
}

test(environment_refuses_a_bad_name)
{
        fail(setenv(null, text("x"), 1) == -1);
        fail(setenv(text(""), text("x"), 1) == -1);
        fail(setenv(text("HAS=EQUALS"), text("x"), 1) == -1);
        fail(unsetenv(text("HAS=EQUALS")) == -1);
        fail(unsetenv(text("")) == -1);

        return true;
}

test(environment_unset)
{
        fail(setenv(text("DAWNING_GOING"), text("here"), 1) == 0);
        fail(!is_null(getenv(text("DAWNING_GOING"))));
        fail(unsetenv(text("DAWNING_GOING")) == 0);
        fail(is_null(getenv(text("DAWNING_GOING"))));

        //      Removing what is not there is a success, not an error.
        fail(unsetenv(text("DAWNING_GOING")) == 0);

        return true;
}

//      putenv installs the caller's own bytes, so writing through the caller's
//      pointer afterwards changes the variable. That is the contract, and it
//      is the whole reason setenv exists beside it.
static p8 stdlib_test_entry[32] = "DAWNING_HELD=before";

test(environment_putenv_keeps_the_caller_bytes)
{
        fail(putenv(stdlib_test_entry) == 0);
        fail(string_compare(getenv(text("DAWNING_HELD")), text("before")) == 0);

        stdlib_test_entry[13] = 'a';
        stdlib_test_entry[14] = 'f';
        stdlib_test_entry[15] = 't';
        stdlib_test_entry[16] = 'e';
        stdlib_test_entry[17] = 'r';
        stdlib_test_entry[18] = end;

        fail(string_compare(getenv(text("DAWNING_HELD")), text("after")) == 0);

        //      No equals in it means remove that name, which is what glibc
        //      does and what putenv("TZ") is asking for.
        fail(putenv(text("DAWNING_HELD")) == 0);
        fail(is_null(getenv(text("DAWNING_HELD"))));

        return true;
}

test(environment_clear)
{
        fail(setenv(text("DAWNING_TWO"), text("x"), 1) == 0);
        fail(clearenv() == 0);
        fail(is_null(getenv(text("DAWNING_TWO"))));
        fail(is_null(stdlib_environment_list()[0]));

        //      An emptied environment is still a working one.
        fail(setenv(text("DAWNING_THREE"), text("y"), 1) == 0);
        fail(string_compare(getenv(text("DAWNING_THREE")), text("y")) == 0);

        return true;
}

/*
        The growth path, which the handful of variables above never reaches.

        The vector starts with eight spare slots and the arena hands out sixty
        four kilobyte chunks, so nothing until here has made either of them
        reallocate. Two hundred names with four hundred byte values reallocate
        the vector five times over and roll the arena more than once, and the
        check afterwards is that every one of them is still readable -- a
        growth that got its arithmetic wrong truncates the environment and
        every test that only ever set three variables would still pass.
*/
test(environment_grows_past_its_first_vector)
{
        p8 name[32];
        p8 value[512];
        positive index;
        positive fill;

        for (fill = 0; fill < sizeof(value) - 1; fill++)
                value[fill] = (p8)('a' + (fill % 26));

        value[sizeof(value) - 1] = end;

        for (index = 0; index < 200; index++)
        {
                positive length = 0;

                memory_copy_apart(name, text("DAWNING_MANY_"), 13);
                length = 13 + positive_into(name + 13, index);
                name[length] = end;

                //      A different length each time, so the arena is asked
                //      for a different size on every call and a rollover
                //      lands in the middle of a string rather than tidily
                //      between two.
                value[100 + (index % 300)] = end;

                fail(setenv(name, value, 1) == 0);

                value[100 + (index % 300)] = (p8)('a' + ((100 + (index % 300)) % 26));
        }

        for (index = 0; index < 200; index++)
        {
                positive length = 0;
                string_address found;

                memory_copy_apart(name, text("DAWNING_MANY_"), 13);
                length = 13 + positive_into(name + 13, index);
                name[length] = end;

                found = getenv(name);

                fail(!is_null(found));
                fail(string_length(found) == 100 + (index % 300));
                fail(found[0] == 'a');
        }

        //      And they come out again, which walks the shift in unsetenv
        //      over a vector that is no longer the one it started on.
        for (index = 0; index < 200; index += 2)
        {
                positive length = 0;

                memory_copy_apart(name, text("DAWNING_MANY_"), 13);
                length = 13 + positive_into(name + 13, index);
                name[length] = end;

                fail(unsetenv(name) == 0);
        }

        for (index = 0; index < 200; index++)
        {
                positive length = 0;

                memory_copy_apart(name, text("DAWNING_MANY_"), 13);
                length = 13 + positive_into(name + 13, index);
                name[length] = end;

                //      Braced on both sides on purpose: fail() expands to a
                //      bare if, and an unbraced else after it binds to that
                //      inner if instead of this one.
                if (index % 2 == 0)
                {
                        fail(is_null(getenv(name)));
                }
                else
                {
                        fail(!is_null(getenv(name)));
                }
        }

        //      What the kernel gave is still there under all of that.
        fail(!is_null(program_environment_list()[0]));

        return true;
}

#define SORT_MOST 2048

static b32 sort_narrow[SORT_MOST];
static b64 sort_wide[SORT_MOST];

typedef struct
{
        p8 bytes[5];
} sort_odd;

static sort_odd sort_odds[SORT_MOST];

static b32 sort_compare_narrow(address_any left, address_any right)
{
        b32 a = *(b32 address_to)left;
        b32 b = *(b32 address_to)right;

        return a < b ? -1 : (a > b ? 1 : 0);
}

static b32 sort_compare_wide(address_any left, address_any right)
{
        b64 a = *(b64 address_to)left;
        b64 b = *(b64 address_to)right;

        return a < b ? -1 : (a > b ? 1 : 0);
}

static b32 sort_compare_odd(address_any left, address_any right)
{
        return memory_compare(((sort_odd address_to)left)->bytes,
                              ((sort_odd address_to)right)->bytes, 5);
}

//      The comparator qsort_r is given, which reads its answer's direction out
//      of the context rather than out of the code.
static b32 sort_compare_directed(address_any left, address_any right,
                                 address_any context)
{
        b32 order = sort_compare_narrow(left, right);

        return *(b32 address_to)context < 0 ? -order : order;
}

//      The five shapes, and a sixth with only four distinct values so runs of
//      equal keys are everywhere rather than nowhere.
static b64 sort_shaped(b32 shape, positive index, positive count, b32 draw)
{
        switch (shape)
        {
        case 0:
                return draw;
        case 1:
                return (b64)index;
        case 2:
                return (b64)(count - index);
        case 3:
                return 42;
        case 4:
                return (b64)(index * 2 < count ? index : count - index);
        default:
                return draw % 4;
        }
}

test(sort_every_shape_at_every_width)
{
        static const positive counts[] = {0, 1, 2, 3, 5, 8, 12, 13, 17,
                                          41, 64, 257, 1000, 2048};
        positive which;
        b32 seed;

        for (seed = 1; seed <= 8; seed++)
        {
                srand((p32)seed);

                for (which = 0; which < 14; which++)
                {
                        positive count = counts[which];
                        b32 shape;

                        for (shape = 0; shape < 6; shape++)
                        {
                                positive index;
                                positive total = 0;
                                positive after = 0;

                                for (index = 0; index < count; index++)
                                {
                                        b64 value = sort_shaped(shape, index, count,
                                                                rand());
                                        b32 byte;

                                        sort_narrow[index] = (b32)value;
                                        sort_wide[index] = value;

                                        for (byte = 0; byte < 5; byte++)
                                                sort_odds[index].bytes[byte] =
                                                        (p8)((value >> (byte * 5)) &
                                                             0xff);

                                        total += (positive)(p32)value;
                                }

                                qsort(sort_narrow, count, sizeof(b32),
                                      sort_compare_narrow);
                                qsort(sort_wide, count, sizeof(b64), sort_compare_wide);
                                qsort(sort_odds, count, sizeof(sort_odd),
                                      sort_compare_odd);

                                for (index = 0; index < count; index++)
                                {
                                        after += (positive)(p32)sort_narrow[index];

                                        if (index == 0)
                                                continue;

                                        //      Ordered, at all three widths.
                                        fail(sort_narrow[index - 1] <=
                                             sort_narrow[index]);
                                        fail(sort_wide[index - 1] <= sort_wide[index]);
                                        fail(memory_compare(
                                                     sort_odds[index - 1].bytes,
                                                     sort_odds[index].bytes, 5) <= 0);
                                }

                                //      And still the same elements: a sort
                                //      that drops one is ordered too.
                                fail(total == after);
                        }
                }
        }

        return true;
}

test(sort_leaves_the_degenerate_alone)
{
        sort_narrow[0] = 7;

        qsort(sort_narrow, 0, sizeof(b32), sort_compare_narrow);
        qsort(sort_narrow, 1, sizeof(b32), sort_compare_narrow);
        qsort(null, 10, sizeof(b32), sort_compare_narrow);
        qsort(sort_narrow, 10, 0, sort_compare_narrow);
        qsort(sort_narrow, 10, sizeof(b32), null);

        fail(sort_narrow[0] == 7);

        return true;
}

test(sort_with_a_context)
{
        b32 direction;
        positive index;

        for (index = 0; index < 100; index++)
                sort_narrow[index] = (b32)((index * 37) % 100);

        direction = 1;
        qsort_r(sort_narrow, 100, sizeof(b32), sort_compare_directed,
                address_of direction);

        for (index = 1; index < 100; index++)
                fail(sort_narrow[index - 1] <= sort_narrow[index]);

        direction = -1;
        qsort_r(sort_narrow, 100, sizeof(b32), sort_compare_directed,
                address_of direction);

        for (index = 1; index < 100; index++)
                fail(sort_narrow[index - 1] >= sort_narrow[index]);

        return true;
}

test(search_finds_and_misses)
{
        positive index;

        for (index = 0; index < 100; index++)
                sort_narrow[index] = (b32)(index * 2);

        for (index = 0; index < 100; index++)
        {
                b32 key = (b32)(index * 2);
                b32 address_to found = (b32 address_to)bsearch(
                        address_of key, sort_narrow, 100, sizeof(b32),
                        sort_compare_narrow);

                fail(!is_null(found));
                fail(*found == key);
        }

        for (index = 0; index < 100; index++)
        {
                b32 key = (b32)(index * 2 + 1);

                fail(is_null(bsearch(address_of key, sort_narrow, 100, sizeof(b32),
                                     sort_compare_narrow)));
        }

        {
                b32 key = 0;

                //      Nothing to search is a miss, not a fault.
                fail(is_null(bsearch(address_of key, sort_narrow, 0, sizeof(b32),
                                     sort_compare_narrow)));
        }

        return true;
}

test(division_keeps_the_sign_of_the_numerator)
{
        div_t narrow = div(-7, 2);
        ldiv_t wide = ldiv(-7, 2);
        lldiv_t widest = lldiv(7, -2);

        fail(narrow.quot == -3 && narrow.rem == -1);
        fail(wide.quot == -3 && wide.rem == -1);
        fail(widest.quot == -3 && widest.rem == 1);

        return true;
}

/*
        The generator, against glibc's own numbers.

        srand and srandom are the same generator in glibc -- measured, for
        every seed tried -- and a seed of zero is folded onto one, which is
        why the first two lists below are identical.
*/
test(random_is_glibc_number_for_number)
{
        static const b32 from_one[] = {1804289383, 846930886,  1681692777,
                                       1714636915, 1957747793, 424238335,
                                       719885386,  1649760492};
        static const b32 from_two[] = {1505335290, 1738766719, 190686788,
                                       260874575,  747983061,  906156498,
                                       1502820864, 142559277};
        static const b32 from_many[] = {383100999,  858300821, 357768173,
                                        455528251,  133005921, 116285904,
                                        591987137,  102557902};
        positive index;

        srand(1);

        for (index = 0; index < 8; index++)
                fail(rand() == from_one[index]);

        srand(0);

        for (index = 0; index < 8; index++)
                fail(rand() == from_one[index]);

        srandom(2);

        for (index = 0; index < 8; index++)
                fail(random() == from_two[index]);

        srandom(12345);

        for (index = 0; index < 8; index++)
                fail(random() == from_many[index]);

        srand(1);

        //      Nothing it produces is outside what RAND_MAX promises.
        for (index = 0; index < 4096; index++)
        {
                b32 drawn = rand();

                fail(drawn >= 0 && drawn <= RAND_MAX);
        }

        return true;
}

test(random_defaults_to_seed_one)
{
        static const b32 expected[] = {1804289383, 846930886, 1681692777,
                                       1714636915, 1957747793, 424238335,
                                       719885386, 1649760492};

        /* This case must remain before every case that calls srand. */
        for (positive index = 0; index < 8; index++)
                fail(random() == expected[index]);

        return true;
}

test(random_reentrant_is_glibc_too)
{
        static const b32 expected[] = {476707713, 1186278907, 505671508,
                                       2137716191, 936145377};
        p32 seed = 1;
        positive index;

        for (index = 0; index < 5; index++)
                fail(rand_r(address_of seed) == expected[index]);

        return true;
}

test(atexit_refuses_what_it_cannot_hold)
{
        fail(atexit(null) == -1);
        fail(at_quick_exit(null) == -1);

        return true;
}

test(system_runs_a_command)
{
        //      Nonzero when there is a shell to run things with, which there
        //      has to be for the next two answers to mean anything.
        fail(system(null) != 0);

        fail(wait_status_code((positive)system(text("exit 9"))) == 9);
        fail(wait_status_code((positive)system(text("exit 0"))) == 0);

        return true;
}

test_case test_cases[] = {
        case(exit_runs_handlers_and_flushes),
        case(exit_runs_handlers_in_reverse),
        case(quick_exit_does_not_flush),
        case(immediate_exit_does_not_flush),
        case(abort_raises_sigabrt),
        case(abort_beats_an_ignored_signal),
        case(abort_beats_a_blocked_signal),
        case(an_unnamed_way_of_leaving_reaches_the_trap),
        case(environment_reads_what_the_kernel_gave),
        case(environment_set_then_get),
        case(environment_refuses_a_bad_name),
        case(environment_unset),
        case(environment_putenv_keeps_the_caller_bytes),
        case(environment_clear),
        case(environment_grows_past_its_first_vector),
        case(random_defaults_to_seed_one),
        case(sort_every_shape_at_every_width),
        case(sort_leaves_the_degenerate_alone),
        case(sort_with_a_context),
        case(search_finds_and_misses),
        case(division_keeps_the_sign_of_the_numerator),
        case(random_is_glibc_number_for_number),
        case(random_reentrant_is_glibc_too),
        case(atexit_refuses_what_it_cannot_hold),
        case(system_runs_a_command),
        {null, null},
};

b32 main(void)
{
        log_direct(str("stdlib tests\n\n"));

        test_cases_walk(test_cases);

        //      Deliberately through exit rather than a return, because this
        //      is the family that knows the difference.
        stdlib_exit(test_report((string_address) "\n"));

        return 1;
}
