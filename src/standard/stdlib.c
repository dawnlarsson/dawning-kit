/*
        Experimental C standard library

        stdlib: leaving, the environment, sorting, searching, division that
        keeps its remainder, and a generator

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_STDLIB
#define STANDARD_MODERN_C_STANDARD_STDLIB

/*
        Guarded out of the kernel build and out of a no-platform build. core.c
        includes this umbrella and library.c sets KERNEL_MODE from __MODULE__,
        so without this the module would pull in a second struct stat, a second
        open and a second errno beside the ones <linux/...> already declares.
        The three families that shipped without this guard were each correct in
        isolation and wrong together; the ones that had it were right.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        This is ordinary C, and it is here rather than in library.c for the
        reason netlink.c gives: library.c and its includes hold declarations
        and assembly and nothing else, and that is checked.

        Nothing below is a hardware floor. An allocator has policy; a sort has
        a strategy; an environment has ownership. Every one of those is a
        decision made once and read the same way on all three architectures,
        and writing it three times in assembly would produce three chances to
        get the policy wrong and no chance to get the machine more right. The
        two places where the machine is actually involved -- the trap that
        ends the process, and the trap that raises a signal at it -- are calls
        into routines library.c already owns.

        The whole family is prefixed stdlib_ where it is ours, and carries the
        C name where C has one.
*/

/*
        The names C spells for itself, defended against a second definition.

        Another family in this tree may reach for div_t or RAND_MAX first, and
        a typedef cannot be tested for the way a macro can, so each one gets a
        guard macro of its own that says the shape has been laid down.
*/
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif

#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

#ifndef STANDARD_DIVISION_TYPES
#define STANDARD_DIVISION_TYPES

typedef struct
{
        b32 quot;
        b32 rem;
} div_t;

typedef struct
{
        b64 quot;
        b64 rem;
} ldiv_t;

typedef struct
{
        b64 quot;
        b64 rem;
} lldiv_t;

#endif // STANDARD_DIVISION_TYPES

/*
        A bump arena, deliberately not the allocator.

        Everything this file needs to own is environment text: a NAME=value
        string that outlives the call that made it, and the vector of pointers
        that indexes them. The lifetime is the process, the churn is a handful
        of entries in the life of most programs, and the total is measured in
        hundreds of bytes.

        malloc is being written by somebody else in this same round of work.
        Calling it from here would make two families that have to land in the
        same commit to link, so this takes pages from memory() -- the mmap the
        library already owns -- and hands out aligned slices of them. It never
        frees. unsetenv therefore leaks the entry it drops, and overwriting a
        variable leaks the string it replaced. That is the honest cost of not
        depending on a free() that does not exist yet, and the arena is a
        dozen lines that a later commit can replace with the real allocator
        without any caller noticing.

        Sixty four kilobytes at a time because that is one mmap for an
        environment far larger than any program here will build, and a program
        that sets a variable in a loop gets geometric behaviour rather than a
        syscall per set.
*/
#define STDLIB_ARENA_CHUNK 65536
#define STDLIB_ARENA_ALIGN 16

//      mmap reports failure by returning a small negative errno in the place
//      an address would have gone, so anything in the last page is not memory.
#define stdlib_memory_failed(address) ((positive)(address) > (positive) - 4096)

static p8 address_to stdlib_arena_next = null;
static positive stdlib_arena_left = 0;

static address_any stdlib_arena_take(positive size)
{
        address_any given;

        size = (size + (STDLIB_ARENA_ALIGN - 1)) & ~(positive)(STDLIB_ARENA_ALIGN - 1);

        if (size > stdlib_arena_left)
        {
                positive want = STDLIB_ARENA_CHUNK;
                p8 address_to block;

                while (want < size)
                        want += want;

                block = (p8 address_to)memory(want);

                if (is_null(block) || stdlib_memory_failed(block))
                        return null;

                //      Whatever was left of the previous chunk is abandoned.
                //      It is at most one allocation's worth and chasing it
                //      would need a free list this file has no other use for.
                stdlib_arena_next = block;
                stdlib_arena_left = want;
        }

        given = stdlib_arena_next;
        stdlib_arena_next += size;
        stdlib_arena_left -= size;

        return given;
}

/*
        The environment, and why it has to be copied before it can be changed.

        The kernel hands a process its environment on the stack it starts on:
        a vector of pointers ending in null, sitting above argv, in memory
        that cannot be extended because the argument strings are immediately
        after it. program_environment_list() returns that vector. Reading it
        is fine forever. Adding an entry to it is not possible at all.

        So the first call that could change anything copies the vector -- the
        pointers, not the strings, which are perfectly good where they are --
        into arena memory with room to grow, and every call after that reads
        and writes the copy. getenv reads the copy too, which is the part that
        is easy to get backwards: a getenv still looking at the kernel vector
        would never see a setenv, and would look right in every test that did
        not do both.

        string_get_environment is the library's own lookup and it is the one
        used here, against our vector rather than the kernel's. It compares a
        name against an entry where the entry starts and stops at the equals,
        which is exactly the match this needs.
*/
#define STDLIB_ENVIRONMENT_SLACK 8

static string_address address_to stdlib_environment_vector = null;
static positive stdlib_environment_count = 0;
static positive stdlib_environment_room = 0;

/*
        environ, which is this vector under the name every program knows.

        Every other C name in this tree is attached to a prose one by an alias
        or by a wrapper, and neither is available here, because this is an
        object rather than a routine: the name has to be the object.

        It was left out until now, and the reason has just stopped being true.
        Nothing ran before main, so nothing could point this anywhere, and a
        program written the ordinary way -- for (walk = environ; *walk; walk++)
        -- would have dereferenced a null on its first line. That is worse than
        not offering the name at all, which is why it was not offered. The
        umbrella's startup shim is the C startup that was missing, it runs
        before main, and it publishes the kernel's own vector here, so the loop
        above works from the first line of the first program that writes it.

        Publishing is not one act. Two calls in this file install a new vector:
        the copy stdlib_environment_own takes before anything can be changed,
        and the growth that runs when that copy fills up. Both end by
        publishing, so a program that reads environ, calls setenv and reads it
        again is looking at the vector setenv is actually using rather than at
        a pointer that went stale the first time the environment was written
        to. unsetenv and clearenv move entries about inside a vector they were
        already given and leave its address alone, so they publish nothing,
        which is correct rather than an omission.

        What is deliberately not followed is a program assigning to environ
        itself. POSIX allows it, and it is how a caller replaces a whole
        environment in one move; getenv here would go on reading the vector
        this file owns and would not see it. Following it would put a second
        pointer comparison in every lookup, paid for by everybody, against the
        chance that one caller did the one thing almost nobody does. It is
        written down instead.
*/
string_address address_to environ = null;

static fn stdlib_environment_publish(void)
{
        environ = stdlib_environment_vector;
}

//      Take a private copy of the vector, once. False means the arena could
//      not give us the room, and every caller falls back to read-only
//      behaviour against the kernel's own vector rather than losing entries.
static bool stdlib_environment_own(void)
{
        string_address address_to kernel;
        string_address address_to made;
        positive count = 0;
        positive room;

        if (!is_null(stdlib_environment_vector))
                return true;

        kernel = program_environment_list();

        if (!is_null(kernel))
                while (!is_null(kernel[count]))
                        count++;

        room = count + STDLIB_ENVIRONMENT_SLACK;
        made = (string_address address_to)stdlib_arena_take((room + 1) *
                                                            sizeof(string_address));

        if (is_null(made))
                return false;

        //      The pointers and not the strings, which stay where the
        //      kernel put them. memory_copy_apart is the non-overlapping
        //      copy and these two are a fresh arena slice and the kernel's
        //      own vector, which cannot be the same memory.
        if (count > 0)
                memory_copy_apart(made, kernel, count * sizeof(string_address));

        made[count] = null;

        stdlib_environment_vector = made;
        stdlib_environment_count = count;
        stdlib_environment_room = room;

        stdlib_environment_publish();

        return true;
}

//      Room for one more entry beside the null that ends the vector. Growth
//      abandons the old vector in the arena; it is a few dozen bytes and the
//      alternative is a free() this file has decided not to require.
static bool stdlib_environment_grow(void)
{
        string_address address_to made;
        positive room;

        if (stdlib_environment_count + 1 <= stdlib_environment_room)
                return true;

        room = stdlib_environment_room + stdlib_environment_room / 2 +
               STDLIB_ENVIRONMENT_SLACK;
        made = (string_address address_to)stdlib_arena_take((room + 1) *
                                                            sizeof(string_address));

        if (is_null(made))
                return false;

        /* The new arena slice cannot overlap the live vector. Copy its
           pointers and terminating null through the architecture's bulk
           core instead of retiring a load, store, increment and branch for
           every entry here. */
        memory_copy_apart(made, stdlib_environment_vector,
                          (stdlib_environment_count + 1) *
                              sizeof(string_address));

        stdlib_environment_vector = made;
        stdlib_environment_room = room;

        stdlib_environment_publish();

        return true;
}

//      The index of the entry whose key is exactly this name, or -1. It is
//      library.common.c's NAME= question asked of every entry in turn. The
//      older spelling here compared first and
//      looked for the equals afterwards, which answers differently for a name
//      that carries an equals of its own -- the very case this comment says
//      can never be found -- and which reads `length` bytes of an entry whose
//      key is shorter than that before deciding it does not match.
static PURE bipolar stdlib_environment_find(string_address name, positive length)
{
        positive index;

        for (index = 0; index < stdlib_environment_count; index++)
                if (environment_key_is(stdlib_environment_vector[index],
                                       name, length))
                        return (bipolar)index;

        return -1;
}

//      A name is a name only if it is not empty and holds no equals: the
//      equals is the separator, and a name carrying one would produce an
//      entry nothing could ever look up again.
static bool stdlib_environment_name_valid(string_address name, positive address_to length)
{
        if (is_null(name))
                return false;

        string_address stop = string_first_of_or_end(name, '=');

        if (stop == name || stop[0] == '=')
                return false;

        *length = (positive)(stop - name);
        return true;
}

//      The live vector, kernel's or ours, for anyone who needs to hand a whole
//      environment to execve. This is what environ would have been; see the
//      note at system() below for why the variable itself is not defined here.
string_address address_to stdlib_environment_list(void)
{
        if (stdlib_environment_own())
                return stdlib_environment_vector;

        return program_environment_list();
}

string_address getenv(string_address name)
{
        if (is_null(name) || name[0] == end)
                return null;

        if (stdlib_environment_own())
                return string_get_environment(stdlib_environment_vector, name);

        return string_get_environment(program_environment_list(), name);
}

static inline INLINE fn stdlib_environment_install(bipolar found,
                                                   string_address entry)
{
        if (found >= 0)
        {
                stdlib_environment_vector[found] = entry;
                return;
        }

        stdlib_environment_vector[stdlib_environment_count++] = entry;
        stdlib_environment_vector[stdlib_environment_count] = null;
}

/*
        setenv copies both halves into storage it owns, because the caller is
        entitled to free or overwrite either one the instant this returns.

        Zero for overwrite means "only if it is not already there", and the
        already-there case is a success that changes nothing rather than a
        refusal: a program that sets a default and then reads it back gets the
        value that was already in the environment, which is the point.
*/
b32 setenv(string_address name, string_address value, b32 overwrite)
{
        positive name_length;
        positive value_length;
        bipolar found;
        p8 address_to entry;

        if (!stdlib_environment_name_valid(name, address_of name_length))
                return -1;

        if (is_null(value))
                value = (string_address) "";

        if (!stdlib_environment_own() || !stdlib_environment_grow())
                return -1;

        found = stdlib_environment_find(name, name_length);

        if (found >= 0 && !overwrite)
                return 0;

        value_length = string_length(value);
        entry = (p8 address_to)stdlib_arena_take(name_length + value_length + 2);

        if (is_null(entry))
                return -1;

        memory_copy_apart(entry, name, name_length);
        entry[name_length] = '=';
        memory_copy_apart(entry + name_length + 1, value, value_length);
        entry[name_length + 1 + value_length] = end;

        stdlib_environment_install(found, entry);
        return 0;
}

/*
        unsetenv removes every entry with this key, not just the first.

        A duplicate key in the vector is not something this file can create,
        but it is something a kernel or an exec'ing parent can hand us, and
        removing one of two would leave a variable that getenv still finds
        after it has been unset. The loop is over an environment of a few
        dozen entries and runs once.
*/
b32 unsetenv(string_address name)
{
        positive name_length;
        positive index = 0;

        if (!stdlib_environment_name_valid(name, address_of name_length))
                return -1;

        if (!stdlib_environment_own())
                return -1;

        while (index < stdlib_environment_count)
        {
                if (environment_key_is(stdlib_environment_vector[index],
                                       name, name_length))
                {
                        //      Everything above the entry moves down one
                        //      place, the null that ends the vector included,
                        //      which is why the count is the distance to the
                        //      end and not one less. memory_copy is memmove
                        //      -- library.c aliases both names onto it -- so
                        //      the overlap is the routine's business.
                        memory_copy(stdlib_environment_vector + index,
                                    stdlib_environment_vector + index + 1,
                                    (stdlib_environment_count - index) *
                                            sizeof(string_address));

                        stdlib_environment_count--;
                        continue;
                }

                index++;
        }

        return 0;
}

/*
        putenv keeps the caller's string, and that is not an oversight.

        POSIX says the string becomes part of the environment: a later write
        through the caller's own pointer changes the variable, and freeing it
        while it is still installed is the caller's bug rather than ours. It
        is the one entry point here that stores something the arena did not
        make, which is why setenv is the one to reach for and this exists for
        the code that already expects it.

        A string with no equals in it is a request to remove that name, which
        is what glibc does and what a caller writing putenv("TZ") means.
*/
b32 putenv(string_address entry)
{
        positive name_length;
        string_address stop;
        bipolar found;

        if (is_null(entry))
                return -1;

        //      The same one scan the name check above uses. Where it stopped
        //      is the key's length and what it stopped on says whether there
        //      is a value at all.
        stop = string_first_of_or_end(entry, '=');

        if (stop[0] != '=')
                return unsetenv(entry);

        name_length = (positive)(stop - entry);

        if (name_length == 0)
                return -1;

        if (!stdlib_environment_own() || !stdlib_environment_grow())
                return -1;

        found = stdlib_environment_find(entry, name_length);

        stdlib_environment_install(found, entry);
        return 0;
}

/*
        An environment with nothing in it is still a vector with a null in it,
        because everything that walks one stops on that null and not on a
        pointer that was never written.

        The entries it drops are abandoned rather than reclaimed, like every
        other drop here. Nothing leaks that the arena made on the first call,
        because those pointers were the kernel's own strings, but a program
        that clears and refills in a loop grows the arena without bound. That
        is the same missing free() the arena's own note is about and it goes
        away with it.
*/
b32 clearenv(void)
{
        if (!stdlib_environment_own())
                return -1;

        stdlib_environment_count = 0;
        stdlib_environment_vector[0] = null;

        return 0;
}

/*
        Leaving.

        library.c already defines exit, and what it defines is the trap: it
        puts the code in the argument register and calls the kernel, and
        nothing in the process runs afterwards. That is precisely _Exit, and
        it is what _start calls with main's return value.

        C's exit is a different function that happens to share the spelling --
        it runs the atexit handlers in reverse and flushes what is buffered
        before it traps. It cannot be named exit here, because the assembly
        symbol already is, and the C name is attached by a macro instead.

        That macro used to be opt-in, and what it was staying away from was
        real. The shell in this same tree includes this umbrella and calls exit
        inside forked children, most of them after a failed execve. A child
        inherits the parent's four-kilobyte log buffer with whatever the parent
        had not flushed still in it, and a child that flushed would print the
        parent's pending output a second time, on every failed command. glibc
        has exactly that bug and answers it with folklore -- flush before you
        fork -- which is advice given to callers rather than a mechanism.

        The mechanism is below, in stdlib_buffers_are_ours, and when output is
        pending it costs one getpid on the one path in a process's life that
        ends the process. With it the macro is the default and the divergence
        is closed at both ends:
        printf then exit prints, which is what every C program on earth
        expects and what this library got wrong, and printf then fork then
        exit in the child prints nothing extra, which is better than what
        glibc does. STANDARD_EXIT_KEEPS_TRAP goes back the other way for a
        caller that wants the bare trap under the C spelling.

        stdlib_exit is the function under either name and can always be called
        directly.

        Handlers are popped before they are called, so a handler that calls
        exit again finishes the list rather than starting it over, and a
        handler that registers another one during the walk gets it run: both
        are undefined behaviour in the standard and both are cheaper to make
        harmless than to detect.

        The flush hook exists because streams belong to another family. exit
        must close what fopen opened, and this file must not name fopen. A
        null function pointer that the stream family fills in costs one load
        and one test at the only moment in a process's life where nothing is
        in a hurry.
*/
#define STDLIB_EXIT_HANDLERS 64

typedef fn(address_to stdlib_exit_handler)(void);

static stdlib_exit_handler stdlib_exit_list[STDLIB_EXIT_HANDLERS];
static positive stdlib_exit_count = 0;

static stdlib_exit_handler stdlib_quick_list[STDLIB_EXIT_HANDLERS];
static positive stdlib_quick_count = 0;

//      Set by the stream family, if there is one linked. Called after the
//      atexit handlers and before the trap.
fn(address_to stdlib_exit_flush_hook)(void) = null;

/*
        Whose bytes are in the buffer.

        A buffer is a promise to write something later, and a fork copies the
        promise without copying the obligation. After one there are two
        processes each holding the same unwritten bytes, and each of them
        leaving through a path that flushes writes those bytes once, so they
        are written twice. That is the double print, and it is not a shell
        problem: it is what buffered output plus fork means, here and in glibc
        alike.

        So the flush asks whose bytes these are before it writes them, and the
        question is asked of the buffer rather than of the process. A stream
        records whose process filled it at the moment it went from empty to
        holding something -- the owner field in struct stream -- and the flush
        at exit compares that against the identity below. A child of a process
        that flushed before it forked starts with an empty buffer, stamps its
        own writes with itself, and its output goes out at exit exactly as a
        program with no fork in it would expect. A child that inherited bytes
        it never wrote finds somebody else's stamp and leaves them where they
        are; the parent is still holding the same bytes and will write them
        itself.

        The identity is one getpid, 32ns on the machine this was written on,
        and a stream asks for it only when its buffer goes from empty to
        holding something -- once per four kilobytes for a program writing
        steadily. Through fwrite to a real file, in milliseconds, lower is
        quicker:

              chunk    rounds    without      with
                 64   2000000       20.9      22.0
               4096    200000      105.3     109.8

        Four to five percent, and the four kilobyte column is exactly the two
        hundred thousand getpids it makes. Against /dev/null the same stamp is
        fifteen percent at sixty four bytes and thirty eight at four thousand,
        which is worth printing because it is the shape of the cost rather
        than the size of it: what is bought is one syscall per buffer, and it
        only looks large standing beside the cheapest syscall Linux has.

        A page mapped MADV_WIPEONFORK answers the same question with a byte
        load and no syscall at all. It was tried, it works, and it is not here:
        it is an mmap, an madvise, a generation counter and a fallback for the
        kernels that refuse, to recover four percent on a path nothing in this
        tree writes through -- the shell's output goes to the log buffer and
        not to a stream, and every consumer that would pay the four percent is
        hypothetical.

        The log buffer in any.inc is coarser, because it has to be. It is
        assembly, and library.c holds assembly and declarations and nothing
        else, so there is nowhere in it to keep a stamp and no C to keep one
        from. What is available is the identity recorded at startup, which
        answers a weaker question -- is this the process the program started
        as -- and that is what gates the log flush below. A forked child's
        implicit flush therefore leaves the log buffer alone whether or not
        the bytes in it are its own. That is exactly the behaviour every such
        child had before this file learned to flush at all, so nothing that
        used to arrive stops arriving, and an explicit log_flush in a child
        still works and is what the shell in this tree does at all twenty of
        its exit sites and before all of its clones.

        The residue, named rather than hidden: a child that writes bytes of
        its own into a stream buffer that already held its parent's loses its
        own along with the inherited ones, because the two are in one buffer
        with nothing between them to tell them apart. glibc writes both and
        then writes the parent's again when the parent exits. Neither answer
        is right; this one never writes anything twice and never invents
        output that was already written, and the way to have both halves is
        the one POSIX has always given -- flush before the fork, or leave
        through _exit.

        A recorded identity of zero means no shim ran -- a kernel build, or a
        translation unit that asked for no platform under it -- and then the
        buffers can only be ours, because nothing in such a build forked.
*/
static positive stdlib_process_at_start = 0;

positive stdlib_process_identity(void)
{
        return (positive)system_call(syscall(getpid));
}

static bool stdlib_buffers_are_ours(void)
{
        if (stdlib_process_at_start == 0)
                return true;

        return stdlib_process_identity() == stdlib_process_at_start;
}

/*
        What the startup shim in the umbrella calls before main, and the only
        thing in this file whose ordering matters.

        Two acts, neither of which can be a static initialiser and both of
        which a program can observe on its very first line: the identity the
        log flush above compares against, and the environment vector under its
        C name.

        The vector published here is the kernel's own and not a copy.
        stdlib_environment_own has not run and cannot have -- nothing has
        called setenv, because nothing has run -- and calling it here to be
        tidy would make every program that includes this umbrella pay for an
        allocation and a copy of its whole environment at startup, in order to
        publish a pointer that reading alone never needed. The moment anything
        does take that copy, that call publishes the copy over this one.
*/
fn stdlib_program_starting(void)
{
#ifdef LINUX
        stdlib_process_at_start = program_initial_identity();
#else
        stdlib_process_at_start = stdlib_process_identity();
#endif

        environ = program_environment_list();
}

static inline INLINE b32 stdlib_handler_add(stdlib_exit_handler handler,
                                            stdlib_exit_handler list[],
                                            positive address_to count)
{
        if (is_null(handler) || address_to count >= STDLIB_EXIT_HANDLERS)
                return -1;

        list[address_to count] = handler;
        address_to count += 1;

        return 0;
}

b32 atexit(stdlib_exit_handler handler)
{
        return stdlib_handler_add(handler, stdlib_exit_list,
                                  address_of stdlib_exit_count);
}

b32 at_quick_exit(stdlib_exit_handler handler)
{
        return stdlib_handler_add(handler, stdlib_quick_list,
                                  address_of stdlib_quick_count);
}

DEAD_END fn stdlib_exit(b32 code)
{
        while (stdlib_exit_count > 0)
        {
                stdlib_exit_count--;
                stdlib_exit_list[stdlib_exit_count]();
        }

        //      The hook decides per stream, because a stream knows which
        //      process filled it. It is called after the handlers rather than
        //      before them, so that anything a handler wrote is included.
        if (!is_null(stdlib_exit_flush_hook))
                stdlib_exit_flush_hook();

        //      An empty log has neither bytes nor an owner to decide. Test
        //      that published state before asking the kernel whose process
        //      this is, so a silent program does not pay a getpid syscall on
        //      its way out. A nonempty log keeps the fork-safe ownership
        //      decision described above unchanged.
        if (log_writer_buffer_length != 0 && stdlib_buffers_are_ours())
                log_flush();

        exit(code);

        __builtin_unreachable();
}

//      quick_exit deliberately does not flush. C11 added it for the program
//      that has decided its buffered state is not worth writing out, and a
//      flush here would take that choice away from the only caller who ever
//      asks for it.
DEAD_END fn quick_exit(b32 code)
{
        while (stdlib_quick_count > 0)
        {
                stdlib_quick_count--;
                stdlib_quick_list[stdlib_quick_count]();
        }

        exit(code);

        __builtin_unreachable();
}

#ifndef STANDARD_NO_UNDERSCORE_EXIT
DEAD_END fn _Exit(b32 code)
{
        exit(code);

        __builtin_unreachable();
}
#endif

#ifndef STANDARD_NO_UNDERSCORE_EXIT
DEAD_END fn _exit(b32 code) __attribute__((alias("_Exit")));
#endif

/*
        abort, which has to win.

        The standard says abort terminates, and it says so without an escape:
        a SIGABRT handler that returns does not get to keep the process
        running. So this raises it three times over, each attempt weaker in
        what it trusts and stronger in what it forces.

        First the signal is unblocked and raised with whatever disposition the
        program installed, so a handler that wants to write a message gets to.
        If that returns -- because the handler returned, or because SIGABRT
        was ignored and the raise did nothing at all -- the disposition is
        reset to the default and it is raised again, and the default action
        for SIGABRT is to die.  If even that returns, which needs the kernel
        to have refused both calls, the process leaves by the trap with 127.

        The sigaction handed to the kernel is four zeroed words and it is the
        same four words on every architecture here, which is worth saying
        because the struct is not. x86_64 carries a restorer pointer between
        the flags and the mask and arm64 and riscv64 do not, so the mask lands
        at a different offset on each. Every field is zero -- SIG_DFL is zero,
        no flags, an empty mask -- so a zeroed buffer is the right buffer
        under both layouts, and there is no need to write the struct twice.
        A restorer would be needed to install a real handler on x86_64, and
        this never installs one, which is exactly why it does not need one.

        tgkill rather than kill so the signal is delivered to this thread and
        cannot be taken by another one that has SIGABRT blocked.
*/
#ifndef SIGABRT
#define SIGABRT 6
#endif

#define STDLIB_SIGNAL_UNBLOCK 1
#define STDLIB_SIGNAL_SET_BYTES 8

static fn stdlib_signal_unblock(b32 number)
{
        positive mask = (positive)1 << (number - 1);

        system_signal_mask(STDLIB_SIGNAL_UNBLOCK, address_of mask, 0,
                           STDLIB_SIGNAL_SET_BYTES);
}

/* The raw default-action shape is shared by abort and namespace launchers;
   it is four zero words on every supported kernel ABI. */
static inline INLINE fn process_signal_default(b32 number)
{
        system_signal_install(number, 0, 0, 0, null);
}

static fn stdlib_signal_raise(b32 number)
{
        bipolar group = system_call(syscall(getpid));
        bipolar thread = system_call(syscall(gettid));

        system_call_3(syscall(tgkill), (positive)group, (positive)thread,
                      (positive)number);
}

DEAD_END fn abort(void)
{
        stdlib_signal_unblock(SIGABRT);
        stdlib_signal_raise(SIGABRT);

        process_signal_default(SIGABRT);
        stdlib_signal_unblock(SIGABRT);
        stdlib_signal_raise(SIGABRT);

        exit(127);

        __builtin_unreachable();
}

/*
        qsort, and why it is an introsort.

        The three candidates were a plain quicksort with median of three, a
        heapsort, and an introsort that is the first until it stops going
        well and the second afterwards.

        Plain quicksort loses on adversarial input, and adversarial input is
        not hypothetical here: the organ pipe -- up to the middle and back
        down again -- is a shape real data takes, it defeats median of three,
        and it turns n log n into n squared with no warning and no bound.
        Heapsort alone never does that but it pays for the guarantee
        everywhere: its comparisons are more numerous and its memory access
        pattern jumps by powers of two, which costs cache misses a partition
        scan does not have. Introsort is the first one's speed with the
        second one's ceiling, at the price of counting the recursion depth,
        and the counter is one decrement per partition.

        The depth limit is twice the base two logarithm of the count. A well
        behaved input halves at every level and finishes in log n, so the
        limit is never reached and heapsort is never entered; an input that
        splits badly enough to spend twice that budget was never going to
        finish as a quicksort, and the switch happens while the partition is
        still large enough for heapsort's guarantee to matter.

        The pivot is chosen by Tukey's ninther once the range is over forty
        elements and by the median of three below that, and the reason is the
        organ pipe: an array that climbs to the middle and comes back down.
        Median of three reads the first, the middle and the last of that and
        finds the minimum, the maximum and the minimum again, so it picks the
        minimum -- the worst pivot available -- and peels one element per
        partition until the depth limit fires and heapsort finishes the job.
        Measured, on two hundred thousand elements, that cost 11.4 million
        comparisons against glibc's 1.9 and was six times slower in time. The
        ninther takes the median of three medians of three, spread across the
        range, and cannot be fooled by a shape that only has three points on
        it. The same input costs 4.5 million comparisons with it.

        The scan is Hoare's, with the pivot parked at the front and both
        halves walking inward. The right scan stops on the pivot itself and
        needs no bound; the left scan carries one, because the ninther's pick
        can be larger than the last element and there is then nothing at the
        far end to stop it. That is one predictable compare per step of one of
        the two loops, against an indirect call in the same step, and buying
        the sentinel back would mean giving up the ninther.

        Both scans stop on an element equal to the pivot rather than stepping
        over it, which is what makes the all-equal input split down the middle
        instead of degenerating. Scanning with <= instead of < is the classic
        mistake here and it turns every run of equal keys into the worst case.

        Small ranges are finished by insertion sort. The cutoff is twelve,
        which is where a partition costs more in bookkeeping than the
        insertion it would save.

        On the retpoline question this codebase raises: ASM_CALL in library.c
        routes an indirect call through __x86_indirect_thunk_ only in a kernel
        build on x86_64 with the retpoline mitigation configured, and that is
        assembly asking for a thunk the compiler would have supplied on its
        own. qsort is C, so the comparator call is emitted by the compiler and
        obeys whatever -mindirect-branch the build asks for; nothing here has
        to ask. What the indirect call does change is the shape of the answer
        rather than the choice of algorithm: the comparator is by far the most
        expensive thing in the loop, so the sort is worth measuring by
        comparison count and not by swap count, and that is what picked
        Sedgewick's partition and the insertion cutoff. The measurement of the
        call's cost is in the notes with the benchmark.

*/
#ifndef STDLIB_SORT_SMALL
#define STDLIB_SORT_SMALL 12
#endif

#define STDLIB_SORT_NINTHER 40

typedef b32(address_to stdlib_compare)(address_any left, address_any right);
typedef b32(address_to stdlib_compare_context)(address_any left, address_any right,
                                               address_any context);

typedef struct
{
        stdlib_compare_context compare;
        address_any context;
        positive size;
} stdlib_sort_plan;

static bool stdlib_sort_before(p8 address_to left, p8 address_to right,
                               stdlib_sort_plan address_to plan)
{
        return plan->compare(left, right, plan->context) < 0;
}

//      Which of the three belongs in the middle. Returned as a position
//      rather than a value, because an element here is an opaque run of bytes
//      of a width only the caller knows.
static p8 address_to stdlib_sort_median(p8 address_to first, p8 address_to second,
                                        p8 address_to third,
                                        stdlib_sort_plan address_to plan)
{
        if (stdlib_sort_before(first, second, plan))
        {
                if (stdlib_sort_before(second, third, plan))
                        return second;

                return stdlib_sort_before(first, third, plan) ? third : first;
        }

        if (stdlib_sort_before(first, third, plan))
                return first;

        return stdlib_sort_before(second, third, plan) ? third : second;
}

static fn stdlib_sort_insertion(p8 address_to base, positive count,
                                stdlib_sort_plan address_to plan)
{
        positive size = plan->size;
        positive index;

        for (index = 1; index < count; index++)
        {
                p8 address_to walk = base + index * size;

                while (walk > base && stdlib_sort_before(walk, walk - size, plan))
                {
                        memory_exchange_apart(walk, walk - size, size);
                        walk -= size;
                }
        }
}

//      Sift one element down a heap whose root is at index zero. The child
//      chosen is the larger of the two, so the element that rises is the one
//      that belongs above the other.
static fn stdlib_sort_sift(p8 address_to base, positive root, positive count,
                           stdlib_sort_plan address_to plan)
{
        positive size = plan->size;

        for (;;)
        {
                positive child = root * 2 + 1;

                if (child >= count)
                        return;

                if (child + 1 < count &&
                    stdlib_sort_before(base + child * size,
                                       base + (child + 1) * size, plan))
                        child++;

                if (!stdlib_sort_before(base + root * size, base + child * size, plan))
                        return;

                memory_exchange_apart(base + root * size, base + child * size,
                                      size);
                root = child;
        }
}

static fn stdlib_sort_heap(p8 address_to base, positive count,
                           stdlib_sort_plan address_to plan)
{
        positive size = plan->size;
        positive build = count / 2;

        while (build > 0)
        {
                build--;
                stdlib_sort_sift(base, build, count, plan);
        }

        while (count > 1)
        {
                count--;
                memory_exchange_apart(base, base + count * size, size);
                stdlib_sort_sift(base, 0, count, plan);
        }
}

/*
        The quicksort half. It recurses into the smaller partition and loops
        on the larger, which bounds the stack at the base two logarithm of the
        count no matter how badly the splits go -- the same reason the depth
        counter is about work and not about safety.
*/
static fn stdlib_sort_range(p8 address_to base, positive count,
                            stdlib_sort_plan address_to plan, positive budget)
{
        positive size = plan->size;

        while (count > STDLIB_SORT_SMALL)
        {
                p8 address_to low;
                p8 address_to high;
                p8 address_to middle;
                p8 address_to pivot;
                p8 address_to left;
                p8 address_to right;
                positive taken;

                if (budget == 0)
                {
                        stdlib_sort_heap(base, count, plan);
                        return;
                }

                budget--;

                low = base;
                high = base + (count - 1) * size;
                middle = base + (count / 2) * size;

                if (count > STDLIB_SORT_NINTHER)
                {
                        positive step = (count / 8) * size;

                        pivot = stdlib_sort_median(
                                stdlib_sort_median(low, low + step,
                                                   low + step * 2, plan),
                                stdlib_sort_median(middle - step, middle,
                                                   middle + step, plan),
                                stdlib_sort_median(high - step * 2, high - step,
                                                   high, plan),
                                plan);
                }
                else
                {
                        pivot = stdlib_sort_median(low, middle, high, plan);
                }

                //      The pivot is parked at the front, where the right scan
                //      finds it and stops. Nothing stands at the other end,
                //      which is what the bound in the left scan is for.
                memory_exchange_apart(pivot, low, size);
                pivot = low;

                left = low;
                right = high + size;

                for (;;)
                {
                        do
                                left += size;
                        while (left <= high && stdlib_sort_before(left, pivot, plan));

                        do
                                right -= size;
                        while (stdlib_sort_before(pivot, right, plan));

                        if (left >= right)
                                break;

                        memory_exchange_apart(left, right, size);
                }

                memory_exchange_apart(low, right, size);

                taken = (positive)(right - base) / size;

                if (taken < count - taken - 1)
                {
                        stdlib_sort_range(base, taken, plan, budget);
                        base = right + size;
                        count = count - taken - 1;
                }
                else
                {
                        stdlib_sort_range(right + size, count - taken - 1, plan,
                                          budget);
                        count = taken;
                }
        }

        stdlib_sort_insertion(base, count, plan);
}

fn qsort_r(address_any base, positive count, positive size,
           stdlib_compare_context compare, address_any context)
{
        stdlib_sort_plan plan;
        positive budget;

        if (is_null(base) || is_null(compare) || size == 0 || count < 2)
                return;

        plan.compare = compare;
        plan.context = context;
        plan.size = size;

        //      Inline bsr/clz; the RISC-V floor owns its multi-step search.
#if RISCV64
        budget = 63 - (positive)bits_leading_zeros(count);
#else
        budget = (positive)top_bit_known(count);
#endif

        stdlib_sort_range((p8 address_to)base, count, address_of plan, budget * 2);
}

/*
        qsort_r's comparator takes a context and qsort's does not. Holding the
        plain pointer in that context keeps both function types honest; the
        forwarder is one load and a tail call on all three architectures.
*/
typedef struct
{
        stdlib_compare plain;
} stdlib_compare_holder;

static b32 stdlib_compare_forward(address_any left, address_any right,
                                  address_any context)
{
        return ((stdlib_compare_holder address_to)context)->plain(left, right);
}

fn qsort(address_any base, positive count, positive size, stdlib_compare compare)
{
        stdlib_compare_holder holder;

        //      qsort_r refuses a null comparator, and passing the cast one
        //      straight down used to be how that refusal was reached. The
        //      forwarder is never null, so the same guard has to be here or
        //      a null comparator becomes a call through null instead of a
        //      sort that declines. The test that caught this passes exactly
        //      that, together with a zero count, a zero width and a null
        //      base, and it is the only reason this line exists.
        if (is_null(compare))
                return;

        holder.plain = compare;

        qsort_r(base, count, size, stdlib_compare_forward, address_of holder);
}

/*
        bsearch, which compares key against element and never the other way
        round. The order matters: a comparator written for qsort is often
        asymmetric in what it accepts, and the C standard fixes the key as the
        left argument.

        The narrowing is by half of the remaining count rather than by a
        midpoint index, so nothing here can overflow on a table larger than
        half the address space, and the empty table falls out of the loop
        condition rather than needing a case of its own.
*/
address_any bsearch(address_any key, address_any base, positive count,
                    positive size, stdlib_compare compare)
{
        p8 address_to table = (p8 address_to)base;

        if (is_null(key) || is_null(base) || is_null(compare) || size == 0)
                return null;

        while (count > 0)
        {
                positive half = count / 2;
                p8 address_to middle = table + half * size;
                b32 order = compare(key, middle);

                if (order == 0)
                        return middle;

                if (order > 0)
                {
                        table = middle + size;
                        count = count - half - 1;
                        continue;
                }

                count = half;
        }

        return null;
}

/*
        Division that keeps both halves.

        C guarantees the quotient truncates toward zero and that the remainder
        has the sign of the numerator, and both of those have been true of the
        language's own operators since C99, so the whole of this is one divide
        instruction on every architecture here -- the compiler emits the pair
        from a single division on x86_64, and a divide followed by a multiply
        and subtract on arm64 and riscv64, which have no remainder instruction.
        Writing it by hand would produce the same instructions and would have
        to be written three times to say so.
*/
#define STDLIB_DIVIDE(name, type, result)                                    \
        CONST result name(type numerator, type denominator)                  \
        {                                                                    \
                result answer = {numerator / denominator,                    \
                                 numerator % denominator};                   \
                return answer;                                               \
        }

STDLIB_DIVIDE(div, b32, div_t)
STDLIB_DIVIDE(ldiv, b64, ldiv_t)
STDLIB_DIVIDE(lldiv, b64, lldiv_t)
#undef STDLIB_DIVIDE

/*
        rand, and the decision to be bit for bit what glibc is.

        glibc's rand and random are the same generator -- measured, not
        assumed: srand(n) and srandom(n) produce identical sequences for every
        seed tried, and an unseeded random() produces the sequence srandom(1)
        produces. So one implementation carries all four names, and the
        verification is not "it looks random" but "it is the same numbers",
        which is a test that either passes or points at a line.

        The generator is an additive feedback one: thirty one words of state,
        the word three ahead added into the word being read, and the sum's top
        thirty one bits handed back. Its period is 2^31 - 1 times 2^30 or
        thereabouts, which is enormous, and its low bits are the ones an
        additive generator is weakest in, which is why the answer is the sum
        shifted right rather than the sum masked.

        The seeding is the Park-Miller multiplicative generator run thirty
        times to fill the state, written with Schrage's factoring so the
        intermediate never leaves thirty one bits -- 16807 times a value up to
        2^31 would need 46 bits, and the original this reproduces was written
        for a machine that did not have them. Then three hundred and ten
        values are drawn and thrown away, which is ten times the state size,
        so the first number a caller sees does not carry the shape of the
        seed.

        None of this is cryptographic and none of it should be used as if it
        were. A program that needs unpredictable bytes wants getrandom, which
        is a syscall and not this.
*/
#define STDLIB_RANDOM_DEGREE 31
#define STDLIB_RANDOM_SEPARATION 3
#define STDLIB_RANDOM_WARMUP (STDLIB_RANDOM_DEGREE * 10)

#define STDLIB_RANDOM_MODULUS 2147483647
#define STDLIB_RANDOM_MULTIPLIER 16807
#define STDLIB_RANDOM_QUOTIENT 127773
#define STDLIB_RANDOM_REMAINDER 2836

/* The state after srandom(1)'s 310 warm-up draws.  C requires no particular
   default seed; glibc specifies one, and it is seed one.  Keeping that exact
   post-warm state in the image makes the first draw identical without making
   every later draw test whether the first one has happened.  Regenerating
   these words through srandom(1) produces the same array and leaves the two
   indices at 3 and 0 because 310 is ten complete turns of the ring. */
static b32 stdlib_random_state[STDLIB_RANDOM_DEGREE] = {
        -1726662223, 379960547, 1735697613, 1040273694, 1313901226,
        1627687941, -179304937, -2073333483, 1780058412, -1989503057,
        -615974602, 344556628, 939512070, -1249116260, 1507946756,
        -812545463, 154635395, 1388815473, -1926676823, 525320961,
        -1009028674, 968117788, -123449607, 1284210865, 435012392,
        -2017506339, -911064859, -370259173, 1132637927, 1398500161,
        -205601318};
static positive stdlib_random_front = STDLIB_RANDOM_SEPARATION;
static positive stdlib_random_rear = 0;

b32 random(void)
{
        p32 sum = (p32)stdlib_random_state[stdlib_random_front] +
                  (p32)stdlib_random_state[stdlib_random_rear];

        stdlib_random_state[stdlib_random_front] = (b32)sum;

        stdlib_random_front++;

        if (stdlib_random_front == STDLIB_RANDOM_DEGREE)
                stdlib_random_front = 0;

        stdlib_random_rear++;

        if (stdlib_random_rear == STDLIB_RANDOM_DEGREE)
                stdlib_random_rear = 0;

        return (b32)(sum >> 1);
}

fn srandom(p32 seed)
{
        positive index;

        //      Zero would make the multiplicative seeding produce nothing but
        //      zero forever, so it is folded onto one, which is why srand(0)
        //      and srand(1) give the same sequence in every libc.
        if (seed == 0)
                seed = 1;

        stdlib_random_state[0] = (b32)seed;

        for (index = 1; index < STDLIB_RANDOM_DEGREE; index++)
        {
                b64 previous = stdlib_random_state[index - 1];
                b64 high = previous / STDLIB_RANDOM_QUOTIENT;
                b64 low = previous % STDLIB_RANDOM_QUOTIENT;
                b64 word = STDLIB_RANDOM_MULTIPLIER * low -
                           STDLIB_RANDOM_REMAINDER * high;

                if (word < 0)
                        word += STDLIB_RANDOM_MODULUS;

                stdlib_random_state[index] = (b32)word;
        }

        stdlib_random_front = STDLIB_RANDOM_SEPARATION;
        stdlib_random_rear = 0;
        for (index = 0; index < STDLIB_RANDOM_WARMUP; index++)
                random();
}

fn srand(p32 seed) __attribute__((alias("srandom")));
b32 rand(void) __attribute__((alias("random")));

/*
        rand_r, which is a different generator entirely and has to be.

        It has no state of its own, so it cannot be the one above: the caller
        holds a single thirty two bit word and that is the whole of what it
        gets to remember. The answer is built out of three steps of a linear
        congruential generator, eleven bits then ten then ten, taken from the
        middle of each step because the low bits of an LCG with a power of two
        modulus cycle with a period as short as two. This is the sequence
        glibc produces, checked against it.
*/
b32 rand_r(p32 address_to seed)
{
        p32 next = *seed;
        b32 answer;

        next = next * 1103515245 + 12345;
        answer = (b32)((next / 65536) % 2048);

        next = next * 1103515245 + 12345;
        answer <<= 10;
        answer ^= (b32)((next / 65536) % 1024);

        next = next * 1103515245 + 12345;
        answer <<= 10;
        answer ^= (b32)((next / 65536) % 1024);

        *seed = next;

        return answer;
}

/*
        system, which is worth having and is not what POSIX describes.

        What it does have is the shape callers rely on: a shell is started on
        the command, the caller waits for it, and the raw wait status comes
        back, which wait_status_code turns into the number a shell would have
        reported. A null command asks whether there is a shell at all, and the
        answer is taken from whether /bin/sh can be executed rather than
        assumed.

        There is no fork syscall on arm64 or riscv64 -- the asm-generic table
        never had one -- so the child comes from clone with SIGCHLD and no new
        stack, which is what fork is underneath and what every other spawn in
        this tree already calls. With every argument but the flags zero, the
        two argument orders the architectures disagree about cannot be told
        apart.

        What is missing against POSIX is the signal handling: system is
        supposed to ignore SIGINT and SIGQUIT in the parent for the duration
        and block SIGCHLD, so that interrupting the child does not also
        interrupt the caller. Installing a disposition and restoring it needs
        the sigaction round trip that abort above went out of its way not to
        need, and doing it half way would be worse than not doing it. A caller
        that cares should spawn the child itself.
*/
#define STDLIB_SHELL "/bin/sh"

//      access(2)'s X_OK, which is not open(2)'s FILE_EXECUTE.
#define STDLIB_ACCESS_EXECUTE 1

b32 system(string_address command)
{
        string_address words[4];
        bipolar child;
        positive raw = 0;

        if (is_null(command))
        {
                //      Is the shell there and runnable. The number is
                //      access(2)'s own X_OK and not FILE_EXECUTE, which is
                //      open(2)'s flag of the same name and a different value:
                //      asking faccessat for 010 asks about a permission bit
                //      that does not exist and is answered EINVAL. library.c
                //      has the same warning beside memory() about the mmap
                //      flags, and it is the same mistake.
                return system_access_at(AT_FDCWD, STDLIB_SHELL,
                                        STDLIB_ACCESS_EXECUTE) == 0;
        }

        words[0] = (string_address)STDLIB_SHELL;
        words[1] = (string_address) "-c";
        words[2] = command;
        words[3] = null;

        //      Anything this process has buffered belongs to this process.
        //      The child would inherit the buffer and write it out a second
        //      time, so it is written out once, here, before the fork.
        log_flush();

        child = system_fork();

        if (child < 0)
                return -1;

        if (child == 0)
        {
                system_execute((address_any)STDLIB_SHELL, (address_any)words,
                               (address_any)stdlib_environment_list());

                //      execve only returns when it failed, and the shell's
                //      own answer for a command it could not run is 127.
                exit(127);
        }

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
                return -1;

        return (b32)raw;
}

/*
        The C spelling of exit.

        Function-like on purpose, so it expands only where exit is being
        called: the assembly symbol of the same name keeps its address, a bare
        exit still takes it, and every call written above this line in the
        umbrella's include order still means the trap -- error.c's _exit and
        _Exit, abort and quick_exit above, and the execve child inside
        system(), each of which has to mean the trap and nothing else.

        STANDARD_EXIT_KEEPS_TRAP is the way back out, for a caller who wants
        the bare trap under this spelling. Nothing in this tree defines it;
        the guard in stdlib_exit is what made needing it unnecessary.
*/
#ifndef STANDARD_EXIT_KEEPS_TRAP
#define exit(code) stdlib_exit(code)
#endif

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_STDLIB
