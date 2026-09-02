/*
        Experimental C standard library

        spool: the rest of <stdio.h> -- a stream with something behind it the
        stream family does not own. A child process, a temporary name, a
        buffer of bytes, or a file that is about to stop existing.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SPOOL
#define STANDARD_MODERN_C_STANDARD_SPOOL

/*
        What is here and why it is not in stream.c.

        stream.c is a buffer and a descriptor and the state machine between
        them, and every line of it is about that. Nothing in it forks, nothing
        in it invents a filename, and nothing in it knows there is a shell on
        this system. The entries below all do exactly one of those things and
        then hand the result to stream.c, so keeping them apart keeps the file
        that everybody's printf goes through free of process management.

        The order is remove, then the temporary names, then popen and pclose,
        then the small stream entries that had nowhere else to live, then the
        unlocked spellings.

        Two names this family deliberately does not define, because a sibling
        already has them and a second definition would not link: rename is
        error.c line 1517, and setbuf is stream.c line 1728. The shell path
        is stdlib.c's STDLIB_SHELL rather than a second "/bin/sh" written out
        here, so that a system that moves its shell moves it once.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        remove, which is unlink and then rmdir.

        C says remove deletes a file and leaves it implementation-defined
        whether it can delete a directory; POSIX says it must, by being unlink
        for everything that is not a directory and rmdir for one that is.
        There is no syscall that is both, so the question is which one to try
        first, and the answer is unlink -- it is the case that happens, and it
        answers EISDIR for the case that does not, which is a cheap and exact
        signal. Asking stat first would be a second syscall on every call and
        a race besides.

        errno after a failure is whichever call failed last: a missing path
        reports ENOENT from unlink, a non-empty directory reports ENOTEMPTY
        from rmdir. That second one only happens because Linux answers unlink
        on a directory with EISDIR specifically. A kernel that answered EPERM
        instead -- which some do, and which is what the standard permits --
        would never reach rmdir at all and a directory would be unremovable.
        This is the same behaviour glibc has on Linux for the same reason, so
        it is a shared limit rather than a divergence, and widening the test
        to EPERM as well would diverge from the reference on the case where
        EPERM is the true answer.
*/
static b32 spool_remove_path(string_address path)
{
        b32 answer = unlink(path);

        if (answer == 0)
                return 0;

        if (errno == EISDIR)
                return rmdir(path);

        return answer;
}

/*
        Where a temporary file goes, and how many distinct names there are.

        P_tmpdir and TMP_MAX are the two numbers <stdio.h> is required to
        publish. TMP_MAX is 238328, which is 62 cubed and is the count glibc
        publishes, and it is the number of names a six-X template can hold if
        the alphabet is the sixty-two alphanumerics. The alphabet below is
        sixty-four rather than sixty-two, for the reason written above
        spool_name_table, so the real count is larger and TMP_MAX is a floor
        on it rather than an exact figure. A floor is what the standard asks
        for.
*/
#define P_tmpdir "/tmp"
#define TMP_MAX 238328

//      L_tmpnam is the buffer size tmpnam demands of a caller who supplies
//      one: "/tmp/" is five, the prefix "tmp" is three, six X's, and the
//      terminator. Twenty is that with room, and is what glibc publishes.
#define L_tmpnam 20

#define SPOOL_TEMPORARY_PREFIX P_tmpdir "/tmp"
#define SPOOL_TEMPORARY_TEMPLATE SPOOL_TEMPORARY_PREFIX "XXXXXX"

//      The six bytes a template ends in, and the mode a temporary file is
//      created with. 0600 and not 0666: a file whose name was picked to be
//      unguessable should not be readable by everybody the moment it exists.
#define SPOOL_TEMPLATE_MARKS 6
#define SPOOL_TEMPORARY_MODE 0600

/*
        The alphabet, written four times, which is the whole of the random
        name generation.

        A random name is six bytes drawn from an alphabet, and the naive way
        to write it is a loop with letters[byte % 62] in it. Two things are
        wrong with that. It is a hand-rolled byte loop where library.c has
        memory_translate, which is a vectorised table walk that does four
        bytes a round; and the modulus is biased, because 256 is not a
        multiple of 62 and the first eight letters come up one time in
        thirty-two more often than the rest.

        Both go away at once by choosing an alphabet of sixty-four and writing
        it out four times. 64 divides 256 exactly, so table[byte] is
        alphabet[byte & 63] with no bias at all and no arithmetic; and a
        256-byte table is precisely what memory_translate takes. The six
        random bytes go in and six random letters come out, in one call, with
        no loop in this file.

        The two extra characters are underscore and hyphen, both legal in
        every filesystem this runs on and neither of them shell-special in a
        way that matters for a name that is never passed to a shell. A hyphen
        can never lead, because the name always begins with the prefix.
*/
#define SPOOL_ALPHABET                                   \
        "abcdefghijklmnopqrstuvwxyz"                     \
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"                     \
        "0123456789_-"

//      The bound is left for the compiler to count rather than written as
//      256. The four repetitions are exactly 256 bytes and the table is only
//      ever indexed with a byte, so the size was right; what it was not is a
//      string, and saying 256 told gcc to drop the terminator it would
//      otherwise have written. That is a diagnostic under
//      -Wunterminated-string-initialization, which the terminal harness
//      builds with, and the whole tree stopped compiling in that lane for a
//      byte nothing reads. Now it is 257 bytes, one of them a terminator
//      nobody looks at, and every index below is unchanged.
static const p8 spool_name_table[] = SPOOL_ALPHABET SPOOL_ALPHABET
        SPOOL_ALPHABET SPOOL_ALPHABET;

/*
        Six random bytes, from the kernel where it will give them and from the
        clock where it will not.

        getrandom with GRND_NONBLOCK is the right primitive and the flag is
        the important half: without it the call blocks until the entropy pool
        is initialised, which on a machine that has just booted -- which is
        exactly when init and the shell are calling this -- can be seconds.
        src/net/dhcp.c has the same note beside the same syscall and reached
        the same conclusion.

        When it refuses, the fallback is not pretending to be random. It is a
        monotonic clock's nanoseconds, the thread id, and a counter that moves
        on every call, stirred by a multiply-shift so that adjacent
        nanosecond readings do not produce adjacent names. That is enough to
        make a collision unlikely; it is emphatically not enough to make a
        name unguessable, and no caller should treat mkstemp as a security
        boundary on a kernel that has no entropy. What makes mkstemp safe is
        O_EXCL, not the name.
*/
#define SPOOL_RANDOM_NONBLOCK 1
#define SPOOL_CLOCK_MONOTONIC 1

static positive spool_name_counter = 0;

static fn spool_random_bytes(p8 address_to into, positive size)
{
        positive when[2] = {0, 0};
        positive mixed;
        positive index = 0;

        if ((positive)system_call_3(syscall(getrandom), (positive)into, size,
                                    SPOOL_RANDOM_NONBLOCK) == size)
                return;

        system_call_2(syscall(clock_gettime), SPOOL_CLOCK_MONOTONIC,
                      (positive)address_of when);

        spool_name_counter++;

        mixed = when[1] ^ (when[0] << 20) ^
                ((positive)system_call(syscall(gettid)) << 40) ^
                (spool_name_counter * 0x9e3779b97f4a7c15ULL);

        while (index < size)
        {
                mixed ^= mixed >> 33;
                mixed *= 0xff51afd7ed558ccdULL;
                mixed ^= mixed >> 29;

                into[index] = (p8)mixed;
                index++;
        }
}

//      Six random letters into the six bytes a template's X's occupy. One
//      random draw and one memory_translate; see spool_name_table for why
//      there is no loop and no modulus.
static fn spool_name_marks(p8 address_to marks)
{
        spool_random_bytes(marks, SPOOL_TEMPLATE_MARKS);
        memory_translate(marks, SPOOL_TEMPLATE_MARKS,
                         (address_any)spool_name_table);
}

/*
        Where a template's X's are, or -1 with errno set.

        POSIX requires the six X's be the last six characters for mkstemp and
        allows a fixed suffix after them for mkstemps, so the search is
        arithmetic from the end rather than a scan: the caller has told us
        exactly how many bytes follow. string_length and memory_compare are
        library.c's, and memory_compare with a literal six folds to a
        four-byte and a two-byte compare at the call site.
*/
static bipolar spool_template_marks(string_address form, positive suffix)
{
        positive length;

        if (is_null(form))
        {
                errno = EINVAL;
                return -1;
        }

        length = string_length(form);

        if (length < SPOOL_TEMPLATE_MARKS + suffix)
        {
                errno = EINVAL;
                return -1;
        }

        length -= SPOOL_TEMPLATE_MARKS + suffix;

        if (memory_compare((address_any)(form + length), (address_any) "XXXXXX",
                           SPOOL_TEMPLATE_MARKS) != 0)
        {
                errno = EINVAL;
                return -1;
        }

        return (bipolar)length;
}

/*
        The one loop every mkstemp-shaped entry shares.

        Draw six letters, try to create, and on EEXIST draw again. Everything
        else about the four public spellings is arguments: mkdtemp asks for a
        directory instead of a file, mkostemp adds caller flags, mkstemps
        allows a suffix.

        The bound is TMP_MAX because that is the number the standard names,
        but it is not really a bound on work: each attempt is an independent
        draw from an alphabet with 64^6 names in it, so the loop runs once
        unless the directory is astonishingly full. What actually stops a
        broken call quickly is the second test -- any failure that is not
        EEXIST is a real failure and returns at once. Without that, mkstemp in
        an unwritable directory would make two hundred thousand syscalls
        before reporting EACCES.

        O_EXCL is what makes this safe, and it is not optional: it is the
        kernel promising that exactly one of any number of racing creators
        wins. The unguessability of the name is a second line of defence and
        not the first.
*/
static b32 spool_temporary_make(string_address form, positive suffix,
                                b32 flags, bool directory)
{
        bipolar at = spool_template_marks(form, suffix);
        positive attempt = 0;

        if (at < 0)
                return -1;

        while (attempt < TMP_MAX)
        {
                b32 answer;

                spool_name_marks((p8 address_to)form + at);

                if (directory)
                        answer = mkdir(form, 0700);
                else
                        answer = open(form,
                                      O_RDWR | O_CREAT | O_EXCL | flags,
                                      SPOOL_TEMPORARY_MODE);

                if (answer >= 0)
                        return answer;

                if (errno != EEXIST)
                        return -1;

                attempt++;
        }

        errno = EEXIST;
        return -1;
}

//      The four spellings, each one argument away from the loop above.
//      mkdtemp returns the template rather than a descriptor, which is the
//      one of the four that is not a file.
static b32 spool_temporary_open(string_address form)
{
        return spool_temporary_make(form, 0, 0, false);
}

static b32 spool_temporary_open_flagged(string_address form, b32 flags)
{
        return spool_temporary_make(form, 0, flags, false);
}

static b32 spool_temporary_open_suffixed(string_address form, b32 suffix)
{
        if (suffix < 0)
        {
                errno = EINVAL;
                return -1;
        }

        return spool_temporary_make(form, (positive)suffix, 0, false);
}

static string_address spool_temporary_directory(string_address form)
{
        if (spool_temporary_make(form, 0, 0, true) < 0)
                return null;

        return form;
}

/*
        tmpnam, which is the entry in this file that cannot be made good.

        It returns a name that does not exist and then returns, and between
        the returning and the caller's open anybody may create that name. The
        standard knows this; every compiler on earth warns about it; the
        function is still in <stdio.h> and old code still calls it, so it is
        here and it is written to be as close to harmless as it can be: the
        name is drawn from the same unguessable alphabet mkstemp uses, and it
        is checked for non-existence with faccessat rather than assumed.

        A caller who can change is asking for mkstemp, which hands back a
        descriptor to a file it created and never lets a name exist without
        one.

        With a null argument it answers into a static buffer, which is what
        the standard defines and is the other reason the function is bad: two
        calls return the same pointer and the first answer is gone.
*/
static p8 spool_name_storage[L_tmpnam];

static string_address spool_temporary_name(string_address into)
{
        string_address answer = is_null(into)
                                        ? (string_address)spool_name_storage
                                        : into;
        positive attempt = 0;

        while (attempt < TMP_MAX)
        {
                memory_copy((address_any)answer,
                            (address_any)SPOOL_TEMPORARY_TEMPLATE,
                            sizeof(SPOOL_TEMPORARY_TEMPLATE));

                spool_name_marks((p8 address_to)answer +
                                 sizeof(SPOOL_TEMPORARY_TEMPLATE) - 1 -
                                 SPOOL_TEMPLATE_MARKS);

                if (faccessat(AT_FDCWD, answer, F_OK, 0) < 0 &&
                    errno == ENOENT)
                        return answer;

                attempt++;
        }

        return null;
}

/*
        tempnam, tmpnam's other half: a caller-chosen directory and prefix,
        and an answer the caller must free.

        The directory is the first of TMPDIR, the argument, P_tmpdir that is
        writable, which is the order glibc uses. The prefix is at most five
        bytes, also glibc's rule, and the reason is that the whole answer has
        to stay under the historical name limit.

        It has tmpnam's race and adds an allocation to it. It is here because
        old code calls it; new code should not.
*/
#define SPOOL_PREFIX_MAX 5
#define SPOOL_NAME_MAX 255

static string_address spool_temporary_named(string_address directory,
                                            string_address prefix)
{
        string_address chosen = getenv((string_address) "TMPDIR");
        p8 address_to answer;
        positive length;
        positive at;

        if (is_null(chosen) || faccessat(AT_FDCWD, chosen, W_OK, 0) < 0)
                chosen = directory;

        if (is_null(chosen) || faccessat(AT_FDCWD, chosen, W_OK, 0) < 0)
                chosen = (string_address)P_tmpdir;

        length = string_length(chosen);

        if (is_null(prefix))
                prefix = (string_address) "tmp";

        at = string_length(prefix);

        if (at > SPOOL_PREFIX_MAX)
                at = SPOOL_PREFIX_MAX;

        if (length + 1 + at + SPOOL_TEMPLATE_MARKS + 1 > SPOOL_NAME_MAX)
        {
                errno = ENAMETOOLONG;
                return null;
        }

        answer = (p8 address_to)malloc(length + 1 + at +
                                       SPOOL_TEMPLATE_MARKS + 1);

        if (is_null(answer))
        {
                errno = ENOMEM;
                return null;
        }

        memory_copy((address_any)answer, (address_any)chosen, length);
        answer[length] = '/';
        memory_copy((address_any)(answer + length + 1), (address_any)prefix, at);
        memory_fill((address_any)(answer + length + 1 + at), 'X',
                    SPOOL_TEMPLATE_MARKS);
        answer[length + 1 + at + SPOOL_TEMPLATE_MARKS] = end;

        spool_name_marks(answer + length + 1 + at);

        return (string_address)answer;
}

/*
        tmpfile: a stream on a file with no name, deleted when the program
        ends whether or not it ends well.

        Two ways to get one, and the first is strictly better. O_TMPFILE
        creates an inode in a directory without ever linking a name to it, so
        there is no window in which a name exists, nothing to clean up, and
        nothing for another process to open. It arrived in Linux 3.11 and not
        every filesystem implements it -- tmpfs and ext4 do, some do not --
        which is why the answer is tested rather than assumed.

        The fallback is the historical one: mkstemp a name, unlink it
        immediately, and keep the descriptor. The file survives on the open
        descriptor alone and vanishes when it closes. The window between
        create and unlink is small and O_EXCL makes it harmless, but it exists,
        which is why it is second.

        O_TMPFILE is the create bit or-ed with O_DIRECTORY, and O_DIRECTORY is
        the one open flag whose value differs across these three targets --
        library.c line 12598 has 040000 for one and 0200000 for the others,
        with the reason. Naming both here rather than a literal is what keeps
        that difference in one place.
*/
#define SPOOL_O_TMPFILE (O_TMPFILE_CREATE | O_DIRECTORY)

static stream address_to spool_temporary_stream(void)
{
        p8 form[sizeof(SPOOL_TEMPORARY_TEMPLATE)];
        stream address_to answer;
        b32 handle;

        handle = open((string_address)P_tmpdir,
                      O_RDWR | SPOOL_O_TMPFILE, SPOOL_TEMPORARY_MODE);

        if (handle < 0)
        {
                memory_copy((address_any)form,
                            (address_any)SPOOL_TEMPORARY_TEMPLATE,
                            sizeof(SPOOL_TEMPORARY_TEMPLATE));

                handle = spool_temporary_open((string_address)form);

                if (handle < 0)
                        return null;

                unlink((string_address)form);
        }

        answer = stream_adopt(handle, (string_address) "w+");

        if (is_null(answer))
        {
                close(handle);
                return null;
        }

        return answer;
}

/*
        popen and pclose, which are the substance of this file.

        WHAT IS ACTUALLY BUILT

        A pipe, a child, and a FILE on this end of it. For "r" the child's
        standard output is the pipe's write end and the caller reads; for "w"
        the child's standard input is the read end and the caller writes.
        Exactly one end survives in each process, which matters more than it
        looks: a reader that leaves the write end open in its own process
        never sees end of file, because the kernel counts writers and it is
        one of them.

        WHAT IS EXECUTED

        stdlib.c's STDLIB_SHELL, which is "/bin/sh", with -c and the command,
        through execve with this process's environment. That is the same path
        system() takes, deliberately: one spelling of the shell in the tree.
        On this distribution the file at that path is programs/shell.c built
        as sh, which handles -c and is what the test lane runs against.

        WHEN THE SHELL IS NOT THERE

        popen still succeeds. It has to: the fork happened, the pipe exists,
        and by the time execve fails the parent has already returned. The
        child's execve returns, the child leaves with 127 -- the number a
        shell itself uses for a command it could not find, and what POSIX
        requires here -- and the caller sees an immediate end of file on a
        read stream, or EPIPE and SIGPIPE on a write stream. pclose then
        answers a raw status whose exit code is 127. A caller who wants to
        know before spending a fork should call system(null), which asks
        exactly that question and is in stdlib.c.

        THE SIDE TABLE

        struct stream has no field for a child, and adding one would put eight
        bytes into every FILE in the program for a feature almost none of them
        use. So the pairing lives here, in a fixed array. Thirty-two is more
        concurrent pipelines than anything in this tree opens and the array
        costs 512 bytes of .bss; a caller who exhausts it gets EMFILE, which
        is the honest errno for "no more of this resource".

        The table is also what makes the child's cleanup possible. glibc
        closes every other popen'd descriptor in a new child, and so does
        this, for a reason that is a real bug otherwise: a program with two
        pipelines open, where the second child inherits the first child's read
        end, keeps that pipe's writer count above zero forever, and the first
        pipeline never reports end of file. Closing them is one walk of the
        table between the fork and the exec.

        WHAT IS MISSING AGAINST POSIX

        The same thing system() is missing and for the same reason: signal
        handling. POSIX says nothing about popen and SIGINT, but it does say
        pclose must not return until the child is collected, and a caller who
        has installed a SIGCHLD handler that reaps everything will find the
        child already gone and get -1/ECHILD here. That is a real divergence
        from nothing -- glibc has it too -- and it is written down rather than
        worked around.
*/
#define SPOOL_CHILDREN 32

typedef struct
{
        stream address_to handle;
        b32 child;
} spool_pipeline;

static spool_pipeline spool_pipeline_table[SPOOL_CHILDREN];

//      A free slot, or -1. The array is small and walked twice in a
//      pipeline's life, which is cheaper than any structure that would avoid
//      the walk.
#define SPOOL_PIPELINE_SLOT(name, parameters, matches)                       \
static bipolar name parameters                                              \
{                                                                           \
        positive at = 0;                                                    \
        while (at < SPOOL_CHILDREN)                                         \
        {                                                                   \
                if (matches)                                                \
                        return (bipolar)at;                                 \
                at++;                                                       \
        }                                                                   \
        return -1;                                                          \
}
SPOOL_PIPELINE_SLOT(spool_pipeline_free_slot, (void),
                    is_null(spool_pipeline_table[at].handle))
SPOOL_PIPELINE_SLOT(spool_pipeline_slot_of, (stream address_to handle),
                    spool_pipeline_table[at].handle == handle)
#undef SPOOL_PIPELINE_SLOT

//      Run in the child, between the fork and the exec: every pipe belonging
//      to a pipeline this process opened earlier is not this child's business.
//      See the note above about a writer count that never reaches zero.
static fn spool_pipeline_close_others(void)
{
        positive at = 0;

        while (at < SPOOL_CHILDREN)
        {
                if (!is_null(spool_pipeline_table[at].handle))
                        close(stream_descriptor(spool_pipeline_table[at].handle));

                at++;
        }
}

static stream address_to spool_open_process(string_address command,
                                            string_address mode)
{
        string_address words[4];
        b32 pair[2];
        bipolar slot;
        b32 child;
        bool reading;
        b32 pipe_flags = 0;
        positive at;
        stream address_to answer;

        if (is_null(command) || is_null(mode))
        {
                errno = EINVAL;
                return null;
        }

        if (mode[0] == 'r')
                reading = true;
        else if (mode[0] == 'w')
                reading = false;
        else
        {
                errno = EINVAL;
                return null;
        }

        //      The only modifier POSIX 2008 gives popen is 'e', which asks
        //      that the caller's end not survive an exec. Anything else is a
        //      caller who thinks this is fopen.
        at = 1;

        while (mode[at] != end)
        {
                if (mode[at] != 'e')
                {
                        errno = EINVAL;
                        return null;
                }

                pipe_flags = O_CLOEXEC;
                at++;
        }

        slot = spool_pipeline_free_slot();

        if (slot < 0)
        {
                errno = EMFILE;
                return null;
        }

        if (pipe2(pair, pipe_flags) < 0)
                return null;

        /*
                Everything this process has buffered belongs to this process,
                and there are two layers of it here where system() only had
                one. log is library.c's own buffered writer and log_flush
                empties it; the FILE buffers are stream.c's and stream_flush
                with a null argument empties all of them. A child that
                inherits either would write the parent's pending bytes a
                second time. stdlib.c's system() flushes only the first of the
                two, which is a real gap in that function and is noted here
                rather than fixed from a different file.
        */
        log_flush();
        stream_flush(null);

        child = fork();

        if (child < 0)
        {
                close(pair[0]);
                close(pair[1]);
                return null;
        }

        if (child == 0)
        {
                b32 theirs = reading ? pair[1] : pair[0];
                b32 ours = reading ? pair[0] : pair[1];
                b32 wanted = reading ? standard_output_descriptor
                                     : standard_input_descriptor;

                //      The other pipelines go first, before anything is
                //      moved onto descriptor zero or one. Doing it after the
                //      dup2 also works today, but only because no popen'd
                //      stream can be holding descriptor zero or one while
                //      this process still has its own standard streams open
                //      -- an invariant a caller breaks by closing stdout and
                //      then opening two pipelines, at which point the walk
                //      would close the descriptor the dup2 had just
                //      installed. glibc closes them first for this reason and
                //      so does this: then the invariant is not needed at all.
                spool_pipeline_close_others();

                close(ours);

                //      dup2 clears close-on-exec on the new descriptor, which
                //      is what lets the 'e' flag be set on both ends of the
                //      pipe and still leave the child with a usable one.
                if (theirs != wanted)
                {
                        dup2(theirs, wanted);
                        close(theirs);
                }

                words[0] = (string_address)STDLIB_SHELL;
                words[1] = (string_address) "-c";
                words[2] = command;
                words[3] = null;

                execve(words[0], words, stdlib_environment_list());

                //      execve only returns when it failed, and 127 is what a
                //      shell answers for a command it could not run.
                _exit(127);
        }

        close(reading ? pair[1] : pair[0]);

        answer = stream_adopt(reading ? pair[0] : pair[1],
                              reading ? (string_address) "r"
                                      : (string_address) "w");

        if (is_null(answer))
        {
                positive raw = 0;

                close(reading ? pair[0] : pair[1]);
                system_wait4_retry(child, address_of raw, 0, null);
                errno = ENOMEM;
                return null;
        }

        spool_pipeline_table[slot].handle = answer;
        spool_pipeline_table[slot].child = child;

        return answer;
}

/*
        pclose: close this end, wait, and hand back the raw status.

        The raw status and not the exit code, which is the part callers get
        wrong. POSIX says pclose returns the termination status "as returned
        by waitpid", so a child that exited 3 gives 768 and not 3, and a
        caller wanting the number a shell would print passes it through
        library.c's wait_status_code. glibc does the same and this matches it
        byte for byte in the diff.

        The wait retries EINTR, through library.c's system_wait4_retry rather
        than error.c's deliberately non-retrying waitpid. That is the opposite
        choice from the one error.c made and it is right here for the opposite
        reason: error.c's waitpid is a thin wrapper whose caller asked for a
        wait and is entitled to be interrupted out of it, while pclose has
        already closed the stream and has nothing sensible to return if it
        gives up. glibc loops here too.

        The slot is released before the wait, not after, so that a stream is
        never in the table with a child that has been collected -- a second
        pclose on the same pointer answers -1/ECHILD rather than waiting on a
        pid that may by then belong to somebody else.
*/
static b32 spool_close_process(stream address_to handle)
{
        bipolar slot = spool_pipeline_slot_of(handle);
        positive raw = 0;
        b32 child;

        if (is_null(handle) || slot < 0)
        {
                errno = ECHILD;
                return -1;
        }

        child = spool_pipeline_table[slot].child;
        spool_pipeline_table[slot].handle = null;
        spool_pipeline_table[slot].child = -1;

        stream_close(handle);

        if (system_wait4_retry(child, address_of raw, 0, null) < 0)
        {
                errno = ECHILD;
                return -1;
        }

        return (b32)raw;
}

/*
        The three small stream entries that had nowhere else to live.

        setlinebuf is setvbuf's line mode with the library picking the size,
        and setbuffer is the BSD spelling that takes a size where setbuf does
        not. Both are one call, and both are here rather than in stream.c only
        because stream.c had already closed its list of aliases; a future edit
        that moves them there loses nothing.

        __fpurge is the interesting one. It throws away everything a stream is
        holding in either direction without writing any of it, which is what a
        forked child wants before it execs something -- the child inherits the
        parent's buffered bytes and must not emit them -- and what a program
        that has decided its output does not matter any more wants. It is
        deliberately not fflush: fflush writes the bytes out, this drops them.
        Both spellings are given, glibc's void-returning __fpurge and BSD's
        int-returning fpurge, because code in the wild calls both.

        Dropping the state is four assignments and not a call to anything,
        because there is no library routine for "forget", and the fields are
        reachable from here for the reason the file header gives: this is the
        same translation unit as stream.c.
*/
static fn spool_set_line_buffered(stream address_to handle)
{
        stream_set_buffering(handle, null, _IOLBF, BUFSIZ);
}

static fn spool_set_buffer_sized(stream address_to handle,
                                 string_address buffer, sized size)
{
        stream_set_buffering(handle, buffer,
                             is_null(buffer) ? _IONBF : _IOFBF, size);
}

static fn spool_purge(stream address_to handle)
{
        if (is_null(handle))
                return;

        handle->read_head = 0;
        handle->read_tail = 0;
        handle->write_used = 0;
        handle->pushback_used = 0;
}

/*
        fgetpos and fsetpos, and what fpos_t has to be.

        The standard is careful never to say fpos_t is an integer. It is an
        opaque object that records everything needed to restore a position,
        which on a library with multibyte state would include the conversion
        state as well as the offset. There is no such state here -- this
        library has no mbstate and every stream is bytes -- so the object is
        the offset and nothing else, and it is a struct rather than a typedef
        of bipolar precisely so that a caller cannot do arithmetic on it and
        then be surprised when a future version has two fields.

        The field is prefixed like everything else in this family, which is
        invisible to a conforming caller: the standard gives no name to any
        member of fpos_t, so there is nothing to collide with.
*/
typedef struct
{
        bipolar spool_offset;
} fpos_t;

static b32 spool_get_position(stream address_to handle,
                              fpos_t address_to into)
{
        bipolar where;

        if (is_null(into))
        {
                errno = EINVAL;
                return -1;
        }

        where = stream_tell(handle);

        if (where < 0)
                return -1;

        into->spool_offset = where;

        return 0;
}

static b32 spool_set_position(stream address_to handle,
                              const fpos_t address_to from)
{
        if (is_null(from))
        {
                errno = EINVAL;
                return -1;
        }

        return stream_seek(handle, from->spool_offset, SEEK_SET);
}

/*
        fmemopen, for reading, and why writing is refused rather than
        approximated.

        Every stream in this library bottoms out at a descriptor. There is no
        vtable in struct stream, no per-stream read and write function
        pointers, and adding them would put two words in every FILE and a
        pair of indirect calls in the hot path of every getc -- to serve a
        function most programs never call. So the way to make a stream out of
        a block of memory without touching stream.c is to give the kernel the
        memory and take a descriptor back, and memfd_create does exactly that:
        an anonymous file that lives in tmpfs, with no name in any directory
        and no cleanup.

        For reading that is exactly right. The bytes are copied in, the offset
        is rewound, and everything stream.c does -- refill, seek, ungetc,
        getline -- works with no special case anywhere.

        For writing it would be exactly wrong, and quietly so. fmemopen's
        write mode promises that the caller's own array receives the bytes and
        that a NUL is maintained after them; a memfd receives them instead and
        the caller's array is never touched. A program would run, produce no
        error, and read back whatever was in its buffer before. That is worse
        than not having the function, so a write mode is refused with EINVAL
        and the reason is this paragraph. Doing it properly means a stream
        that can be flushed into a caller's buffer, which is the same hook
        open_memstream needs; see the note where that one is not defined.

        A kernel without memfd_create -- before 3.17 -- answers ENOSYS and
        this returns null with that errno, which is a true statement about the
        machine.
*/
#define SPOOL_MEMFD_CLOEXEC 1

static stream address_to spool_open_memory(address_any bytes, sized size,
                                           string_address mode)
{
        stream address_to answer;
        bipolar handle;
        bipolar written;

        if (is_null(bytes) || is_null(mode) || mode[0] != 'r')
        {
                errno = EINVAL;
                return null;
        }

        handle = system_call_2(syscall(memfd_create),
                               (positive)(address_any) "fmemopen",
                               SPOOL_MEMFD_CLOEXEC);

        if (handle < 0)
        {
                errno = (b32) - handle;
                return null;
        }

        written = system_write_all((positive)handle, bytes, (positive)size);

        if ((positive)written != (positive)size)
        {
                close((b32)handle);
                errno = EIO;
                return null;
        }

        if (lseek((b32)handle, 0, SEEK_SET) < 0)
        {
                close((b32)handle);
                return null;
        }

        answer = stream_adopt((b32)handle, (string_address) "r");

        if (is_null(answer))
        {
                close((b32)handle);
                errno = ENOMEM;
                return null;
        }

        return answer;
}

/*
        The names <stdio.h> and <stdlib.h> know these by.

        Static aliases put both spellings on the same body. Section collection
        still removes an entry a program never calls, while an address taken
        through either spelling remains the address of the same operation.
*/
static b32 remove(string_address) __attribute__((alias("spool_remove_path")));
static b32 mkstemp(string_address)
        __attribute__((alias("spool_temporary_open")));
static b32 mkostemp(string_address, b32)
        __attribute__((alias("spool_temporary_open_flagged")));
static b32 mkstemps(string_address, b32)
        __attribute__((alias("spool_temporary_open_suffixed")));
static string_address mkdtemp(string_address)
        __attribute__((alias("spool_temporary_directory")));
static string_address tmpnam(string_address)
        __attribute__((alias("spool_temporary_name")));
static string_address tempnam(string_address, string_address)
        __attribute__((alias("spool_temporary_named")));
static stream address_to tmpfile(void)
        __attribute__((alias("spool_temporary_stream")));
static stream address_to popen(string_address, string_address)
        __attribute__((alias("spool_open_process")));
static b32 pclose(stream address_to)
        __attribute__((alias("spool_close_process")));
static fn setlinebuf(stream address_to)
        __attribute__((alias("spool_set_line_buffered")));
static fn setbuffer(stream address_to, string_address, sized)
        __attribute__((alias("spool_set_buffer_sized")));
static fn __fpurge(stream address_to)
        __attribute__((alias("spool_purge")));

static b32 fpurge(stream address_to handle)
{
        if (is_null(handle))
                return -1;

        spool_purge(handle);

        return 0;
}

static b32 fgetpos(stream address_to, fpos_t address_to)
        __attribute__((alias("spool_get_position")));
static b32 fsetpos(stream address_to, const fpos_t address_to)
        __attribute__((alias("spool_set_position")));
static stream address_to fmemopen(address_any, sized, string_address)
        __attribute__((alias("spool_open_memory")));

/*
        The _unlocked family, which in a single-threaded library is the same
        family with a different spelling.

        There is no per-call stream lock, so the matching signatures below
        are aliases of the plain entries. If stream.c gains locking, these
        declarations become the lock-free bodies instead.

        flockfile and funlockfile are consequently no-ops, and ftrylockfile
        succeeds: in a single-threaded library there is no contention.
*/
static fn flockfile(stream address_to handle)
{
        (void)handle;
}

static fn funlockfile(stream address_to handle)
        __attribute__((alias("flockfile")));

static b32 ftrylockfile(stream address_to handle)
{
        (void)handle;

        return 0;
}

static b32 getc_unlocked(stream address_to handle)
        __attribute__((alias("stream_get_byte")));
static b32 fgetc_unlocked(stream address_to handle)
        __attribute__((alias("stream_get_byte")));
static b32 getchar_unlocked(void)
        __attribute__((alias("stream_get_byte_standard")));
static b32 putc_unlocked(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte")));
static b32 fputc_unlocked(b32 byte, stream address_to handle)
        __attribute__((alias("stream_put_byte")));

static b32 putchar_unlocked(b32 byte)
{
        return stream_put_byte(byte, stdout);
}

static b32 fputs_unlocked(string_address text, stream address_to handle)
        __attribute__((alias("stream_put_string")));
static string_address fgets_unlocked(string_address into, b32 limit,
                                     stream address_to handle)
        __attribute__((alias("stream_get_line")));
static sized fread_unlocked(address_any into, sized size, sized count,
                            stream address_to handle)
        __attribute__((alias("stream_read")));
static sized fwrite_unlocked(address_any from, sized size, sized count,
                             stream address_to handle)
        __attribute__((alias("stream_write")));
static b32 fflush_unlocked(stream address_to handle)
        __attribute__((alias("stream_flush")));

static PURE b32 feof_unlocked(stream address_to handle)
        __attribute__((alias("stream_at_end")));
static PURE b32 ferror_unlocked(stream address_to handle)
        __attribute__((alias("stream_failed")));
static fn clearerr_unlocked(stream address_to handle)
        __attribute__((alias("stream_clear_state")));
static PURE b32 fileno_unlocked(stream address_to handle)
        __attribute__((alias("stream_descriptor")));

/*
        open_memstream is deliberately not here.

        It is the one entry on this family's list that cannot be built without
        changing stream.c, and building it wrong is worse than not building
        it. What it promises is a stream whose bytes accumulate in an
        allocation the library grows, with the caller's pointer and length
        variables updated to match every time the stream is flushed or closed.
        The updating is the part that has no home: it is a hook that runs
        inside stream_flush_output, and struct stream has no field to hang one
        on and no code path that would call it.

        Faking it with a memfd, the way fmemopen above is built, fails at
        exactly the promise: the caller's pointer would be updated only at
        close, so a program that flushes and reads the buffer -- which is the
        normal way the function is used -- would read nothing.

        The honest version is three things together: one function pointer in
        struct stream called on flush, one on close, and open_memstream
        supplying both. That is a small edit to stream.c and it would also be
        what makes fmemopen's write modes work, and it should be one change
        rather than two. Until then this family says no and says why.
*/

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SPOOL
