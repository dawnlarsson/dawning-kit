/*
        Experimental C standard library

        directories, the exec family, sleeping, and the small POSIX names
        that sit beside them

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_PROCESS
#define STANDARD_MODERN_C_STANDARD_PROCESS

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason src/standard/error.c states at the same place: core.c
        includes this umbrella, library.c sets KERNEL_MODE from __MODULE__,
        and a module that pulled a second struct dirent, a second DIR and a
        second nanosleep in beside the ones <linux/...> already declares would
        not compile. The three families that shipped without this guard were
        each right on their own and wrong together.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        remove, mkstemp, mkstemps, mkdtemp, tmpnam and tmpfile were defined
        here and are not any more. spool.c owns them.

        Both families were told to build them, which was the brief's fault
        and not either family's, and both did. spool.c is where they belong:
        it is the rest of <stdio.h>, its tmpfile hands back a stream rather
        than a descriptor, and its mkstemp is what its other five are built
        on. This file is directories, exec and sleeping, and it reaches
        spool's copies because spool.c is included before it.
*/


/*
        Where this file sits, and what it is allowed to assume.

        It is included last in src/compiler_memory.c, after error.c,
        allocator.c, text.c, stdlib.c, clock.c, math.c, stream.c and format.c,
        and it depends on all but two of them: getdents64, openat, close,
        lseek, readlink, lstat, getcwd, execve and errno itself come from
        error.c, malloc and free from allocator.c, qsort and abort from
        stdlib.c, CLOCK_REALTIME from clock.c, and fdopen from stream.c.
        Placed any earlier it would see a different errno -- error.c does
        `#undef errno` and then defines it as a call to __errno_location --
        and every wrapper here would set a variable nothing reads.

        Nothing in this file is a new trap. Every syscall it needs already has
        a wrapper in error.c with the translation done once, so what is here
        is the part that is genuinely C: a record stream turned into one entry
        at a time, a PATH walk with the errno rules POSIX gives it, a
        canonicaliser, and an option parser.

        Almost everything is static. A spark program is one translation unit,
        so a directory reader that nothing opens is deleted by the compiler
        and a program that never calls scandir does not carry qsort on its
        account. The exceptions are the four getopt globals and __assert_fail,
        which have to be real symbols because they are what a program and a
        compiler respectively reach for by name.
*/

//      -- what the numbers are ---------------------------------------------

/*
        The bounds, all of them the kernel's own and none of them chosen here.

        PATH_MAX is Linux's 4096 and is what every path buffer below is sized
        to; a path longer than that is refused by the kernel anyway, so a
        buffer of that size cannot lose a path the kernel would have accepted.
        NAME_MAX is 255, which is what d_name holds plus its terminator.

        Guarded one at a time because src/sh/file.c already spells its own
        FILE_PATH_MAX and FILE_NAME_MAX, and because a program that included
        real headers before this one has already been told the same numbers.
*/
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef MAXNAMLEN
#define MAXNAMLEN NAME_MAX
#endif

/*
        The d_type values. DT_DIR, DT_REG, DT_LNK, DT_FIFO, DT_SOCK, DT_CHR
        and DT_BLK are already in src/platform/any.inc and are not repeated;
        the two it does not carry are added here, guarded the same way, so
        that a program switching on d_type has the whole set.
*/
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

#ifndef DT_WHT
#define DT_WHT 14
#endif

//      O_TMPFILE is the create bit error.c already names, or-ed with
//      O_DIRECTORY, whose value differs on arm64 and which library.c already
//      spells correctly for each machine. Writing it out here rather than
//      hard-coding 020200000 is what keeps tmpfile working on arm64.
#ifndef O_TMPFILE
#define O_TMPFILE (O_TMPFILE_CREATE | O_DIRECTORY)
#endif

//      clock_nanosleep's one flag: the request is a point on the clock and
//      not a span from now.
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

//      TMP_MAX, L_tmpnam and P_tmpdir went the way the six temporary-file
//      entries above them went: spool.c defines all three, with the same
//      values and with the arithmetic behind them written out, and the
//      umbrella includes spool.c before this file, so every one of these
//      #ifndef guards was already satisfied by the time it was read.
//      PROCESS_TEMPORARY_DIRECTORY, PROCESS_TEMPORARY_NAME and
//      PROCESS_TEMPLATE_TAIL were this file's own and nothing named them.

/*
        How deep a chain of symbolic links realpath will follow before it
        decides the chain is a loop. Linux itself stops at forty and answers
        ELOOP, and matching the kernel means a path this refuses is a path the
        kernel would have refused too.
*/
#define PROCESS_SYMLINK_DEPTH 40

/*
        The getdents64 buffer, which is the one size in this file that is a
        judgement rather than a constant somebody else fixed.

        Thirty two kilobytes is four pages more than glibc's default, and the
        reason to be generous is that every refill is a syscall while every
        entry taken out of a full buffer is a pointer add. A directory of a
        thousand short names fits in one trap here. It is malloc'd with the
        DIR rather than put on the stack because a spark program's stack is
        the one the kernel gave _start and a caller that opens a directory
        inside a deep recursion should not have to know how big this is.
*/
#define PROCESS_DIRECTORY_BUFFER 32768

/*
        The kernel's record header: an inode, an offset, a two byte length and
        a one byte type, and then the name. Nineteen bytes, and the same on
        all three machines because linux_dirent64 is not one of the structures
        asm-generic rewrote.
*/
#define PROCESS_DIRENT_NAME 256
#define PROCESS_DIRENT_HEADER \
        (sizeof(p64) + sizeof(b64) + sizeof(p16) + sizeof(p8))

//      How many words execl and its two relatives will assemble before they
//      give up. A command line longer than this is past what any caller of
//      the variadic forms writes by hand.
#define PROCESS_ARGUMENT_MAX 1024

/*
        Reading a wait status, which error.c leaves undone.

        error.c has wait, waitpid and wait4 and the three option flags, but
        not the seven macros that say what the number they wrote actually
        means, so every caller in this tree decodes it by hand. The encoding
        is the kernel's and is the same on all three machines: the low seven
        bits are the terminating signal, 0x7f in them means stopped, the next
        eight bits are the exit code or the stopping signal, and bit seven is
        the core dump flag.

        WIFEXITED is written against the low seven bits rather than as
        "status & 0xff" so that a status of 0x7f -- stopped -- is not read as
        an exit, and WTERMSIG masks 0x7f rather than 0xff for the same
        reason. library.c's wait_status_code turns the whole thing into the
        single number a shell reports; these are the pieces underneath it.
*/
#ifndef WIFEXITED
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WTERMSIG(status) ((status) & 0x7f)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) (((b8)(((status) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WCOREDUMP(status) ((status) & 0x80)
#endif

//      -- directories ------------------------------------------------------

/*
        struct dirent, which is the kernel's record and glibc's structure at
        the same time.

        linux_dirent64 is an inode, an offset, a length, a type and a
        variable-length name; glibc's struct dirent on a 64 bit Linux is the
        same four fields followed by char d_name[256]. The two agree byte for
        byte over the header, which is why readdir below can hand back a
        pointer straight into the buffer the kernel filled instead of copying
        every entry into a structure of its own. sizeof is 280 either way.

        The name field is declared at its full 256 bytes because that is the
        ABI a program compiled against real headers expects, and because
        readdir_r has to copy into one. It is emphatically not how much of a
        record in the buffer may be read: the kernel writes exactly d_reclen
        bytes and the last record in a full buffer ends where the data ends,
        so anything that copies out of a record bounds itself by d_reclen and
        never by sizeof(d_name). Getting that backwards reads past what
        getdents64 wrote.
*/
typedef struct dirent
{
        p64 d_ino;
        b64 d_off;
        p16 d_reclen;
        p8 d_type;
        p8 d_name[PROCESS_DIRENT_NAME];
} process_dirent;

/*
        What an open directory is.

        A descriptor, the buffer the kernel fills, how much of it is valid,
        how far into it readdir has walked, and the offset telldir hands back.
        The buffer is last and everything before it is eight bytes wide or
        packs to it, because getdents64 writes records that must be eight byte
        aligned and a misaligned p64 load of d_ino is a fault on riscv64 and
        an unaligned access trap on arm64 with strict alignment. malloc gives
        sixteen byte alignment, so the buffer starts aligned and the kernel's
        own padding keeps every record after it aligned too.

        position is the offset of the entry readdir returned last, which is
        what glibc's telldir reports and is the thing that makes the usual
        idiom work: telldir before reading an entry names that entry, because
        it is the offset the previous one recorded, and seekdir back to it
        followed by readdir hands the same entry over again.
*/
typedef struct process_directory
{
        b32 descriptor;
        b32 process_directory_padding;
        positive used;
        positive at;
        b64 position;
        p8 buffer[PROCESS_DIRECTORY_BUFFER];
} DIR;

/*
        The one place a directory is built, so that opendir and fdopendir
        cannot drift apart.

        fdopendir owns the descriptor it is given -- closedir closes it, and
        there is no dup -- which is what POSIX says and what a caller that
        opened the descriptor with its own flags depends on. It is checked
        with fstat first, because handing a regular file to getdents64 gives
        ENOTDIR from the kernel on the first readdir rather than from
        fdopendir, and a program that tested the return of fdopendir would
        never see it.
*/
static DIR address_to process_directory_open(b32 descriptor)
{
        DIR address_to folder;
        struct stat facts;

        if (fstat(descriptor, address_of facts) < 0)
                return null;

        if (!S_ISDIR(facts.st_mode))
        {
                errno = ENOTDIR;
                return null;
        }

        folder = (DIR address_to)malloc(sizeof(DIR));

        if (is_null(folder))
        {
                errno = ENOMEM;
                return null;
        }

        folder->descriptor = descriptor;
        folder->process_directory_padding = 0;
        folder->used = 0;
        folder->at = 0;
        folder->position = 0;

        return folder;
}

/*
        opendir opens the descriptor itself and closes it again if anything
        after that fails, because a caller who gets a null pointer back has no
        way to close a descriptor it was never told about, and a program that
        opens a thousand directories and checks the result would still die of
        EMFILE.

        O_CLOEXEC because a directory handle held open across an exec is a
        leak into a program that did not ask for it, and O_DIRECTORY so that
        the refusal for a regular file comes from open rather than from the
        first read.
*/
static DIR address_to opendir(string_address path)
{
        b32 descriptor;
        DIR address_to folder;
        b32 saved;

        if (is_null(path))
        {
                errno = EINVAL;
                return null;
        }

        descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (descriptor < 0)
                return null;

        folder = process_directory_open(descriptor);

        if (is_null(folder))
        {
                saved = errno;
                close(descriptor);
                errno = saved;
                return null;
        }

        return folder;
}

static DIR address_to fdopendir(b32 descriptor)
{
        if (descriptor < 0)
        {
                errno = EBADF;
                return null;
        }

        return process_directory_open(descriptor);
}

static b32 closedir(DIR address_to folder)
{
        b32 descriptor;

        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        descriptor = folder->descriptor;
        free(folder);

        return close(descriptor);
}

static b32 dirfd(DIR address_to folder)
{
        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        return folder->descriptor;
}

/*
        Reading one record out of the stream, which is the substance of this
        half of the file.

        getdents64 does not hand back entries, it hands back a block of
        variable-length records packed end to end, each one saying in
        d_reclen how far the next one starts. There is no library routine for
        walking that and there could not be: the stride is data. This is one
        of the four hand-written loops in this file and the only one in the
        directory half.

        Every step is a pointer add. The refill happens when the walk has
        reached the end of what the kernel wrote, and a refill that returns
        zero is the end of the directory -- readdir answers null with errno
        untouched there, which is precisely how a caller tells the end from a
        failure.

        The reclen check is not defensive dressing. A record claiming zero
        bytes would make this loop forever and a record claiming more than the
        buffer holds would walk off the end, and neither is something the
        Linux kernel does -- but this pointer comes from a file system driver
        and the cost of being sure is one compare.
*/
static process_dirent address_to readdir(DIR address_to folder)
{
        process_dirent address_to record;
        b32 taken;

        if (is_null(folder))
        {
                errno = EINVAL;
                return null;
        }

        while (folder->at >= folder->used)
        {
                taken = getdents64(folder->descriptor, folder->buffer,
                                   PROCESS_DIRECTORY_BUFFER);

                if (taken < 0)
                        return null;

                if (taken == 0)
                        return null;

                folder->used = (positive)taken;
                folder->at = 0;
        }

        record = (process_dirent address_to)(folder->buffer + folder->at);

        if (record->d_reclen <= PROCESS_DIRENT_HEADER ||
            folder->at + record->d_reclen > folder->used)
        {
                folder->at = folder->used;
                errno = EIO;
                return null;
        }

        folder->at += record->d_reclen;
        folder->position = record->d_off;

        return record;
}

/*
        readdir_r, which copies rather than pointing and reports through its
        return value rather than through errno.

        The copy is bounded by d_reclen and not by sizeof(process_dirent),
        because the record in the buffer is only d_reclen bytes long and the
        last one in a full buffer ends exactly where the kernel stopped
        writing. A copy of 280 bytes off that record reads past the data.

        A name too long for the caller's structure is ENAMETOOLONG rather than
        a truncation, which is what glibc answers and is the only safe
        answer: the caller sized its buffer from the header and has nowhere to
        put the rest.
*/
static b32 readdir_r(DIR address_to folder, process_dirent address_to into,
                     process_dirent address_to address_to result)
{
        process_dirent address_to record;
        b32 saved;

        if (is_null(folder) || is_null(into) || is_null(result))
                return EINVAL;

        saved = errno;
        errno = 0;
        record = readdir(folder);

        if (is_null(record))
        {
                if (errno != 0)
                {
                        b32 reason = errno;
                        errno = saved;
                        return reason;
                }

                errno = saved;
                address_to result = null;
                return 0;
        }

        errno = saved;

        if (record->d_reclen > PROCESS_DIRENT_HEADER + NAME_MAX + 1)
        {
                address_to result = null;
                return ENAMETOOLONG;
        }

        memory_copy(into, record, record->d_reclen);
        address_to result = into;

        return 0;
}

/*
        rewinddir, telldir and seekdir, which are three views of one number.

        The buffer has to be dropped on both of the two that move, because it
        holds records from wherever the descriptor used to be and the entry
        after a seek is the one the kernel reads next, not the one still
        sitting in memory.

        telldir cannot fail and has nothing to report, which is why it is the
        only one of the three that does not touch errno.
*/
static fn rewinddir(DIR address_to folder)
{
        if (is_null(folder))
                return;

        lseek(folder->descriptor, 0, SEEK_SET);
        folder->used = 0;
        folder->at = 0;
        folder->position = 0;
}

static bipolar telldir(DIR address_to folder)
{
        if (is_null(folder))
        {
                errno = EINVAL;
                return -1;
        }

        return (bipolar)folder->position;
}

static fn seekdir(DIR address_to folder, bipolar where)
{
        if (is_null(folder))
                return;

        if (lseek(folder->descriptor, where, SEEK_SET) < 0)
                return;

        folder->used = 0;
        folder->at = 0;
        folder->position = where;
}

/*
        strverscmp, which is what versionsort is made of.

        This is GNU's algorithm and not an approximation of it, transcribed
        from the state machine glibc uses, because "sorts version numbers
        sensibly" is not a specification and two implementations that both
        sound reasonable disagree on ordinary inputs. The whole of the
        behaviour is in the two tables: which state a digit, a zero or
        anything else moves to, and what the answer is once the two strings
        have diverged.

        The four states are running text, an integral part being compared, a
        fractional part, and a fractional part that is still all zeroes. The
        distinction that matters is that "1.010" sorts before "1.09", because
        a run of digits that begins with a zero is a fraction and compares
        left to right, while a run that begins with one to nine is an integer
        and the longer run is the larger number.

        This is the second of the four hand-written loops here. There is no
        library routine for it and could not be: the comparison is a state
        machine over two strings at once, not a scan of one. byte_is_digit
        does the digit test, which is the part that would otherwise have been
        a hand-rolled range compare.
*/
#define PROCESS_VERSION_NORMAL 0
#define PROCESS_VERSION_INTEGER 3
#define PROCESS_VERSION_FRACTION 6
#define PROCESS_VERSION_ZEROES 9

#define PROCESS_VERSION_COMPARE 2
#define PROCESS_VERSION_LENGTH 3

static const p8 process_version_next[] = {
        //      state         other                    digit                    zero
        /* normal   */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_INTEGER, PROCESS_VERSION_ZEROES,
        /* integer  */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_INTEGER, PROCESS_VERSION_INTEGER,
        /* fraction */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_FRACTION, PROCESS_VERSION_FRACTION,
        /* zeroes   */ PROCESS_VERSION_NORMAL, PROCESS_VERSION_FRACTION, PROCESS_VERSION_ZEROES};

static const b8 process_version_answer[] = {
        //      left over right: other/other other/digit other/zero digit/other
        //      digit/digit digit/zero zero/other zero/digit zero/zero
        /* normal   */
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_LENGTH, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        /* integer  */
        PROCESS_VERSION_COMPARE, -1, -1,
        1, PROCESS_VERSION_LENGTH, PROCESS_VERSION_LENGTH,
        1, PROCESS_VERSION_LENGTH, PROCESS_VERSION_LENGTH,
        /* fraction */
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        /* zeroes   */
        PROCESS_VERSION_COMPARE, 1, 1,
        -1, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE,
        -1, PROCESS_VERSION_COMPARE, PROCESS_VERSION_COMPARE};

static b32 strverscmp(string_address left, string_address right)
{
        string_address one = left;
        string_address two = right;
        p8 first;
        p8 second;
        b32 state;
        b32 difference;

        if (one == two)
                return 0;

        first = address_to one++;
        second = address_to two++;

        state = PROCESS_VERSION_NORMAL + (first == '0') +
                (byte_is_digit(first) != 0);

        while ((difference = (b32)first - (b32)second) == 0)
        {
                if (first == end)
                        return 0;

                state = (b32)process_version_next[state];
                first = address_to one++;
                second = address_to two++;
                state += (first == '0') + (byte_is_digit(first) != 0);
        }

        state = (b32)process_version_answer[state * 3 + (second == '0') +
                                            (byte_is_digit(second) != 0)];

        if (state == PROCESS_VERSION_COMPARE)
                return difference;

        if (state != PROCESS_VERSION_LENGTH)
                return state;

        //      Both runs are integers and both had the same digits up to
        //      here, so whichever run is longer is the larger number.
        while (byte_is_digit(address_to one++))
                if (!byte_is_digit(address_to two++))
                        return 1;

        return byte_is_digit(address_to two) ? -1 : difference;
}

/*
        The two orderings scandir is normally given.

        alphasort is strcoll in POSIX and strcmp in the C locale, and the C
        locale is the only one there is here, so it is string_compare -- the
        assembly, not a loop. versionsort is strverscmp above.

        Both take a pointer to a pointer, because that is the shape qsort
        hands a comparator when the array is an array of pointers, and it is
        the shape POSIX declares.
*/
static b32 alphasort(process_dirent address_to const address_to left,
                     process_dirent address_to const address_to right)
{
        return string_compare((address_to left)->d_name,
                              (address_to right)->d_name);
}

static b32 versionsort(process_dirent address_to const address_to left,
                       process_dirent address_to const address_to right)
{
        return strverscmp((address_to left)->d_name,
                          (address_to right)->d_name);
}

/*
        scandir: every entry that passes a test, in an order the caller picks.

        Each kept entry is copied into its own allocation sized to the name it
        actually has rather than to the 256 bytes the structure declares,
        which is what glibc does and is the difference between a directory of
        ten thousand short names costing 2.8 megabytes and costing about a
        third of that. The caller frees each entry and then the vector, so
        both have to come from the same allocator the program's free reaches,
        which is why these are malloc and not memory_take.

        The vector doubles. Sixteen to begin with because most directories a
        program scans are small, and doubling because the alternative --
        counting the directory first and then reading it again -- races
        against anything writing into it.

        On any failure everything allocated so far is released before the -1,
        because a scandir that half succeeded and leaked the half is worse
        than one that failed.
*/
#define PROCESS_SCANDIR_FIRST 16

static b32 scandir(string_address path,
                   process_dirent address_to address_to address_to list,
                   b32 (address_to keep)(const process_dirent address_to),
                   b32 (address_to order)(process_dirent address_to const address_to,
                                          process_dirent address_to const address_to))
{
        DIR address_to folder;
        process_dirent address_to record;
        process_dirent address_to address_to found = null;
        process_dirent address_to address_to grown;
        positive room = 0;
        positive count = 0;
        positive length;
        positive wanted;
        positive at;
        b32 saved;

        if (is_null(list))
        {
                errno = EINVAL;
                return -1;
        }

        folder = opendir(path);

        if (is_null(folder))
                return -1;

        while (1)
        {
                errno = 0;
                record = readdir(folder);

                if (is_null(record))
                {
                        if (errno != 0)
                                goto process_scandir_failed;

                        break;
                }

                if (!is_null(keep) && !keep(record))
                        continue;

                if (count == room)
                {
                        positive next = room ? room * 2 : PROCESS_SCANDIR_FIRST;

                        grown = (process_dirent address_to address_to)
                                realloc(found, next * sizeof(found[0]));

                        if (is_null(grown))
                        {
                                errno = ENOMEM;
                                goto process_scandir_failed;
                        }

                        found = grown;
                        room = next;
                }

                length = string_length(record->d_name);
                wanted = PROCESS_DIRENT_HEADER + length + 1;

                found[count] = (process_dirent address_to)malloc(wanted);

                if (is_null(found[count]))
                {
                        errno = ENOMEM;
                        goto process_scandir_failed;
                }

                //      The header and the name and its terminator, and
                //      nothing past it. d_reclen is rewritten to what was
                //      actually allocated so a caller that trusts it does not
                //      read into the next block.
                memory_copy(found[count], record, wanted);
                found[count]->d_reclen = (p16)wanted;
                count++;
        }

        closedir(folder);

        //      A directory that matched nothing still hands back a vector, so
        //      that free(list) is always the right thing to do afterwards.
        if (is_null(found))
        {
                found = (process_dirent address_to address_to)
                        malloc(sizeof(found[0]));

                if (is_null(found))
                {
                        errno = ENOMEM;
                        return -1;
                }
        }

        if (!is_null(order) && count > 1)
                qsort(found, count, sizeof(found[0]),
                      (stdlib_compare)order);

        address_to list = found;

        return (b32)count;

process_scandir_failed:
        saved = errno;

        for (at = 0; at < count; at++)
                free(found[at]);

        free(found);
        closedir(folder);
        errno = saved;

        return -1;
}

//      -- the exec family ---------------------------------------------------

/*
        execv and execvpe are the two ends of this, and everything else is one
        of them with the arguments arranged differently.

        execve itself is error.c's, so nothing here traps. What is here is the
        PATH walk, which is the whole reason execvp is a different function
        from execv.
*/
static b32 execv(string_address path, string_address address_to arguments)
{
        return execve(path, arguments, stdlib_environment_list());
}

/*
        The shell retry, which is the rule people leave out.

        A file that is executable but is not something the kernel can load --
        a shell script with no #! line, most often -- fails execve with
        ENOEXEC. Every Unix since the seventh edition has answered that by
        running /bin/sh on it, and a program that relies on execvp to run a
        bare script relies on this. The new argument list is the shell, then
        the path that failed, then everything the caller passed after argv[0].

        The bound is the one divergence from glibc: glibc builds the new list
        in a variable-length array sized from the caller's argc and this uses
        a fixed one, because a spark program's stack is whatever the kernel
        gave _start and an argument list long enough to matter would take the
        stack out rather than fail. Past the bound the ENOEXEC stands, which
        is the honest answer rather than a crash.
*/
static fn process_execute_shell(string_address path,
                                string_address address_to arguments,
                                string_address address_to environment)
{
        string_address words[PROCESS_ARGUMENT_MAX];
        positive count = 0;
        positive at;

        while (!is_null(arguments[count]))
        {
                count++;

                if (count + 2 >= PROCESS_ARGUMENT_MAX)
                        return;
        }

        words[0] = (string_address) "/bin/sh";
        words[1] = path;

        for (at = 1; at < count; at++)
                words[at + 1] = arguments[at];

        words[count + 1] = null;

        execve(words[0], words, environment);
}

/*
        execvpe: the PATH walk, and the errno rules that make it useful.

        A name with a slash in it is not a PATH lookup at all and is handed
        straight to execve, which is what POSIX says and what every shell
        relies on.

        Otherwise each colon-separated piece of PATH gets the name joined onto
        it and gets one execve. What happens when that fails is the substance:

          - EACCES is remembered and the walk continues, because a directory
            earlier on PATH containing an unreadable file of the right name
            must not stop the search -- but if nothing later works, EACCES and
            not ENOENT is what the caller is told, because "there is one but
            you may not run it" is a different problem from "there is none".
          - ENOENT, ENOTDIR and ESTALE mean this directory simply does not
            have it, and the walk continues with nothing remembered.
          - ENODEV and ETIMEDOUT are what some network file systems answer
            instead of ENOENT; glibc skips them for that reason and so does
            this.
          - ENOEXEC gets the shell retry above, and if that returns too then
            the file was found and could not be run, which is an answer and
            not a reason to keep looking.
          - anything else means a file was found and something went wrong
            running it, which the caller wants to hear about immediately.

        All six of those were measured against glibc 2.44 on the build machine
        rather than taken from a table -- a directory of stubs, one
        unreadable, one a script with no #!, one missing, one runnable, walked
        by both implementations.

        PATH unset is "/bin:/usr/bin", which is what confstr(_CS_PATH)
        answers on the build machine and is what glibc's own execvp falls back
        to. It is deliberately not the shell's "/bin:/usr/bin:/", which is a
        different string for a different question.

        An empty piece of PATH means the working directory, which is POSIX and
        is why the name is used unjoined there.

        The PATH split is string_first_of, which is the assembly, and the join
        is path_join, which is also the assembly and already gets the
        separator right whether or not the directory ended in one. Neither is
        a loop here.
*/
#define PROCESS_DEFAULT_PATH "/bin:/usr/bin"

static bool process_execute_keep_looking(b32 reason, bool address_to denied)
{
        switch (reason)
        {
        case EACCES:
                address_to denied = true;
                return true;

        case ENOENT:
        case ENOTDIR:
        case ESTALE:
        case ENODEV:
        case ETIMEDOUT:
                return true;

        default:
                return false;
        }
}

static b32 execvpe(string_address name, string_address address_to arguments,
                   string_address address_to environment)
{
        p8 candidate[PATH_MAX];
        string_address search;
        string_address segment;
        bool denied = false;
        positive name_length;

        if (is_null(name) || string_get(name) == end)
        {
                errno = ENOENT;
                return -1;
        }

        if (!is_null(string_first_of(name, '/')))
        {
                execve(name, arguments, environment);

                if (errno == ENOEXEC)
                        process_execute_shell(name, arguments, environment);

                return -1;
        }

        name_length = string_length(name);

        if (name_length >= PATH_MAX)
        {
                errno = ENAMETOOLONG;
                return -1;
        }

        search = getenv((string_address) "PATH");

        if (is_null(search))
                search = (string_address)PROCESS_DEFAULT_PATH;

        segment = search;

        while (1)
        {
                string_address next = string_first_of(segment, ':');
                positive length = is_null(next)
                                          ? string_length(segment)
                                          : (positive)(next - segment);

                if (length == 0)
                {
                        //      An empty piece is the working directory, and
                        //      the name relative to it is the name itself.
                        string_copy(candidate, name);
                }
                else if (length + name_length + 2 <= PATH_MAX)
                {
                        p8 directory[PATH_MAX];

                        memory_copy(directory, segment, length);
                        directory[length] = end;
                        path_join(candidate, PATH_MAX, directory, name);
                }
                else
                {
                        //      Too long to be a path the kernel would take,
                        //      so it cannot be where the program is.
                        goto process_execute_next;
                }

                execve(candidate, arguments, environment);

                if (errno == ENOEXEC)
                {
                        process_execute_shell(candidate, arguments,
                                              environment);
                        return -1;
                }

                if (!process_execute_keep_looking(errno, address_of denied))
                        return -1;

        process_execute_next:
                if (is_null(next))
                        break;

                segment = next + 1;
        }

        errno = denied ? EACCES : ENOENT;

        return -1;
}

static b32 execvp(string_address name, string_address address_to arguments)
{
        return execvpe(name, arguments, stdlib_environment_list());
}

/*
        The three variadic spellings.

        Each one walks its own argument list into a vector and then calls the
        vector form, which is the only way to build one: a va_list cannot be
        handed to execve. The list ends at the first null, and execle takes
        one more argument after that null, which is the environment.

        A list longer than the bound is refused with E2BIG rather than
        truncated, because a truncated argument list is a different command.
*/
/*
        The list is handed over by address, and that is not a style choice.

        A va_list is an array type on x86_64, so passing one to a helper
        decays to a pointer and the helper's advances are visible to the
        caller afterwards. On arm64 and riscv64 it is a structure passed by
        value, so they are not: the caller's list is still sitting on the
        first variadic argument when the helper returns. execle is the one
        routine here that reads an argument after the helper has run -- the
        environment, past the terminating null -- and with a by-value list it
        read the wrong one and handed execve a pointer that was not a vector
        at all. That failed with EFAULT on two machines out of three and
        worked on the third, which is exactly the shape of bug three
        architecture parity exists to catch, and it was caught that way.
*/
static positive process_execute_gather(string_address words[],
                                       string_address first,
                                       var_args address_to list)
{
        positive count = 0;

        words[count++] = first;

        if (is_null(first))
                return count;

        while (count < PROCESS_ARGUMENT_MAX)
        {
                words[count] = var_list_get(address_to list, string_address);

                if (is_null(words[count]))
                        return count + 1;

                count++;
        }

        return 0;
}

#define PROCESS_EXECL(name, vector)                                          \
        static b32 name(string_address path, string_address first, ...)      \
        {                                                                    \
                string_address words[PROCESS_ARGUMENT_MAX];                  \
                var_args list;                                               \
                var_list(list, first);                                       \
                positive count = process_execute_gather(                     \
                    words, first, address_of list);                           \
                var_list_end(list);                                          \
                if (!count)                                                  \
                {                                                            \
                        errno = E2BIG;                                        \
                        return -1;                                            \
                }                                                            \
                return vector(path, words);                                  \
        }

PROCESS_EXECL(execl, execv)
PROCESS_EXECL(execlp, execvp)
#undef PROCESS_EXECL

static b32 execle(string_address path, string_address first, ...)
{
        string_address words[PROCESS_ARGUMENT_MAX];
        string_address address_to environment;
        var_args list;
        positive count;

        var_list(list, first);
        count = process_execute_gather(words, first, address_of list);

        if (count == 0)
        {
                var_list_end(list);
                errno = E2BIG;
                return -1;
        }

        environment = var_list_get(list, string_address address_to);
        var_list_end(list);

        return execve(path, words, environment);
}

//      -- sleeping ---------------------------------------------------------

/*
        nanosleep, and why the remaining time needs no code here.

        POSIX says that a sleep cut short by a signal reports how much of it
        was left, and the Linux kernel already writes that into the second
        argument before it returns EINTR. So the contract is kept by passing
        the pointer through and not by measuring anything: there is no clock
        read before, no subtraction after, and no window in which a second
        signal could make the answer wrong.

        The structure is library.c's timespec, whose fields are p64. clock.c
        already explains why: it is a duration handed to this call and a
        duration is never negative. A caller that puts a negative second count
        in it gets the same bits the kernel would have seen from a signed
        field, and the kernel answers EINVAL to both.
*/
static b32 nanosleep(timespec address_to request, timespec address_to remaining)
{
        return error_whole(system_call_2(syscall(nanosleep),
                                         (positive)request,
                                         (positive)remaining));
}

/*
        clock_nanosleep is the one call in this file that does not use errno,
        and that is not an oversight.

        POSIX says it returns the error number directly and leaves errno
        alone, which is the opposite of every other name here and is the
        reason it cannot go through error_whole. A program that writes

            if (clock_nanosleep(...) != 0)

        and then reads errno is reading whatever the last failure left there.

        TIMER_ABSTIME makes the request a point on the named clock rather than
        a span from now, and an absolute sleep writes nothing into remaining
        even when a signal cuts it short -- there is nothing to say, because
        the deadline has not moved.
*/
static b32 clock_nanosleep(clockid_t which, b32 flags,
                           timespec address_to request,
                           timespec address_to remaining)
{
        bipolar answer = system_call_4(syscall(clock_nanosleep),
                                       (positive)which, (positive)flags,
                                       (positive)request,
                                       (positive)remaining);

        return error_failed(answer) ? (b32) - answer : 0;
}

/*
        sleep, which cannot be called sleep here.

        library.c already exports an assembly routine of that name taking a
        timespec, three places in this tree call it, and the symbol is in
        every built object. POSIX's sleep takes an unsigned count of seconds
        and returns the seconds it did not sleep, which is a different
        function of a different type with the same name.

        So the C one is a routine with its own name and an opt-in macro that
        renames it, exactly as stdlib.c does for exit under
        STANDARD_EXIT_RUNS_HANDLERS. It is off by default and must stay off:
        src/test/clock.c and src/test/stream_buffering.c both call the
        assembly sleep with a pointer, and a macro that was on by default
        would compile those into passing a pointer as a second count without
        a word of complaint.

        The body is glibc's, deliberately. It sleeps through clock_nanosleep
        with the same structure as both the request and the remainder, so that
        an interrupted sleep leaves the remainder where the return value is
        computed from, and it rounds a part-second remainder up -- a sleep
        with a hundred nanoseconds left reports one second left, because
        reporting zero would tell a caller looping on the result that it had
        finished.
*/
static positive process_sleep_seconds(positive seconds)
{
        timespec span;

        span.tv_sec = seconds;
        span.tv_nsec = 0;

        if (clock_nanosleep(CLOCK_REALTIME, 0, address_of span,
                            address_of span) == 0)
                return 0;

        return span.tv_sec + (span.tv_nsec > 0);
}

#ifdef STANDARD_SLEEP_IS_POSIX
#define sleep(seconds) process_sleep_seconds(seconds)
#endif

/*
        usleep, which is obsolete and is still what half the world writes.

        A count above a million is not an error here, because it is not one in
        glibc either since it stopped being XSI -- the seconds and the
        remainder are split out and the sleep is the whole of it.
*/
static b32 usleep(p32 microseconds)
{
        timespec span;

        span.tv_sec = microseconds / 1000000u;
        span.tv_nsec = (microseconds % 1000000u) * 1000u;

        return nanosleep(address_of span, null);
}

//      -- names in the file system ------------------------------------------

/*
        remove, which is unlink and rmdir behind one name.

        Linux answers EISDIR when unlink is given a directory, so the second
        call is made only then. A missing file is ENOENT from the first call
        and is reported as it stands.
*/


//      The GNU spelling that lets a template keep an extension: the six X are
//      the six characters before the last `suffix` bytes.


/*
        tmpnam, which is racy by construction and is kept because programs
        call it.

        Between the moment this decides a name is free and the moment the
        caller opens it, anything may create it. That is not a defect in this
        implementation, it is what the interface is: there is no way to hand
        back a name and a guarantee at the same time. mkstemp exists for
        callers that need the guarantee, and tmpfile below uses it rather than
        this.

        A null argument gets a pointer into storage this owns, which the next
        call overwrites, and that too is what the interface says.
*/
//      process_temporary_name went with tmpnam; spool.c has both.


/*
        tmpfile: a stream with no name, deleted before it is handed over.

        O_TMPFILE first, because a file that never had a name cannot be found
        by anything and cannot be left behind by a program that dies before
        fclose. Not every file system supports it -- it wants the directory,
        not a path, and older ones answer EOPNOTSUPP or EISDIR -- so the
        fallback is the old way: make a name, open it, unlink it immediately
        and keep the descriptor. The window in which the name exists is the
        few microseconds between those two calls, and the name is a mkstemp
        name rather than a tmpnam one, so nothing can guess it in time.

        fdopen comes from the stream family, which is the only dependency this
        file has on it.
*/

/*
        realpath, and the reason it is a walk rather than a call.

        There is no syscall that answers this. The kernel resolves a path
        every time it is handed one, but it never says what it resolved to,
        so the only way to know is to do the resolution here: take the
        components one at a time, look at each with lstat, and splice a
        symbolic link's target in front of whatever is left whenever one
        turns up.

        Every component must exist. That is what the interface promises and
        what makes the answer usable -- a path that resolved through a
        directory that is not there would be a guess -- so a missing piece is
        ENOENT and a piece that is a file where a directory was needed is
        ENOTDIR, both of which come out of lstat and are passed through.

        `..` is where a naive walk goes wrong. Popping the last component off
        the answer is right, but only after checking that what is being
        popped out of is a directory: /etc/passwd/.. is ENOTDIR and not /etc,
        because the kernel would say so and a program using this to decide
        whether a path is inside a tree would be told the wrong thing. The
        mode of the last resolved component is kept for exactly that test.

        The loop over components is the fourth and last hand-written loop
        here. It is not a scan for a byte -- string_first_of finds the
        separator and that is the assembly -- it is the state that has to be
        carried from one component to the next, and there is no routine for
        that.

        A null second argument allocates, which is the GNU behaviour and is by
        far the more common call. The buffer a caller supplies must be
        PATH_MAX bytes, because there is no way to tell it how much was
        needed and no way to ask.
*/
static string_address realpath(string_address path, string_address into)
{
        p8 answer[PATH_MAX];
        p8 rest[PATH_MAX];
        p8 link[PATH_MAX];
        p8 merged[PATH_MAX];
        struct stat facts;
        positive answer_length = 0;
        positive at = 0;
        positive followed = 0;
        bool last_was_directory = true;
        b32 saved;

        if (is_null(path))
        {
                errno = EINVAL;
                return null;
        }

        if (string_get(path) == end)
        {
                errno = ENOENT;
                return null;
        }

        if (string_length(path) >= PATH_MAX)
        {
                errno = ENAMETOOLONG;
                return null;
        }

        if (path[0] == '/')
        {
                answer[0] = '/';
                answer[1] = end;
                answer_length = 1;
                string_copy(rest, path + 1);
        }
        else
        {
                if (is_null(getcwd(answer, PATH_MAX)))
                        return null;

                answer_length = string_length(answer);
                string_copy(rest, path);
        }

        while (rest[at] != end)
        {
                string_address slash;
                positive piece;

                //      Skip the separators between components, which also
                //      makes "a//b" and "a/b" the same path.
                if (rest[at] == '/')
                {
                        at++;
                        continue;
                }

                slash = string_first_of(rest + at, '/');
                piece = is_null(slash) ? string_length(rest + at)
                                       : (positive)(slash - (rest + at));

                if (piece == 1 && rest[at] == '.')
                {
                        at += piece;
                        continue;
                }

                if (piece == 2 && rest[at] == '.' && rest[at + 1] == '.')
                {
                        if (!last_was_directory)
                        {
                                errno = ENOTDIR;
                                return null;
                        }

                        //      Root's parent is root, which is what the
                        //      kernel does too.
                        if (answer_length > 1)
                        {
                                answer_length = path_head_copy(answer, PATH_MAX,
                                                               answer);
                        }

                        at += piece;
                        continue;
                }

                if (!last_was_directory)
                {
                        errno = ENOTDIR;
                        return null;
                }

                {
                        p8 held[PATH_MAX];

                        if (piece >= PATH_MAX)
                        {
                                errno = ENAMETOOLONG;
                                return null;
                        }

                        memory_copy(held, rest + at, piece);
                        held[piece] = end;

                        //      Remember where the answer was, so that a
                        //      symbolic link can be undone by shortening it
                        //      again rather than by rebuilding it.
                        {
                                positive was = answer_length;

                                answer_length = path_join(answer, PATH_MAX,
                                                          answer, held);

                                if (answer_length == 0 ||
                                    answer_length >= PATH_MAX - 1)
                                {
                                        errno = ENAMETOOLONG;
                                        return null;
                                }

                                if (lstat(answer, address_of facts) < 0)
                                        return null;

                                if (S_ISLNK(facts.st_mode))
                                {
                                        bipolar wrote;

                                        if (++followed > PROCESS_SYMLINK_DEPTH)
                                        {
                                                errno = ELOOP;
                                                return null;
                                        }

                                        wrote = readlink(answer, link,
                                                         PATH_MAX - 1);

                                        if (wrote < 0)
                                                return null;

                                        link[wrote] = end;

                                        //      What is left of the original
                                        //      path goes after the link's
                                        //      target, and the walk starts
                                        //      again from the front of the
                                        //      spliced string.
                                        at += piece;

                                        if (string_length(link) +
                                                    string_length(rest + at) +
                                                    2 >
                                            PATH_MAX)
                                        {
                                                errno = ENAMETOOLONG;
                                                return null;
                                        }

                                        {
                                                p8 address_to tail =
                                                        string_copy_max_end(
                                                                merged, link,
                                                                PATH_MAX - 1);

                                                if (rest[at] != end)
                                                {
                                                        address_to tail++ = '/';
                                                        string_copy_max_end(
                                                                tail,
                                                                rest + at,
                                                                PATH_MAX - 1 -
                                                                        (positive)(tail -
                                                                                   merged));
                                                }
                                        }

                                        string_copy(rest, merged);
                                        at = 0;

                                        if (rest[0] == '/')
                                        {
                                                answer[0] = '/';
                                                answer[1] = end;
                                                answer_length = 1;
                                        }
                                        else
                                        {
                                                answer[was] = end;
                                                answer_length = was;
                                        }

                                        last_was_directory = true;
                                        continue;
                                }

                                last_was_directory =
                                        S_ISDIR(facts.st_mode) ? true : false;
                                at += piece;
                        }
                }
        }

        if (is_null(into))
        {
                into = (string_address)malloc(answer_length + 1);

                if (is_null(into))
                {
                        errno = ENOMEM;
                        return null;
                }

                memory_copy(into, answer, answer_length + 1);
                return into;
        }

        saved = errno;
        memory_copy(into, answer, answer_length + 1);
        errno = saved;

        return into;
}

/*
        basename and dirname, which are already written and are in assembly.

        path_tail_copy and path_head_copy in library.c implement exactly these
        two rules -- trailing separators go except for root, a path with no
        directory has "." for its head -- with the bound checked and the
        answer's length returned. There is nothing to write here but the
        buffer and the two edge cases POSIX names: a null pointer and an empty
        string both answer ".".

        They are the XPG forms from <libgen.h> and not the GNU basename from
        <string.h>, which is a different function: that one never strips a
        trailing slash and never answers ".". This is the one a program that
        includes libgen.h expects.

        POSIX allows either modifying the argument or returning static
        storage, and static storage is chosen because the alternative writes
        into a string literal for every caller who passes one. The next call
        overwrites it, which POSIX says it may.

        Note that library.c also has path_basename, which is a different
        routine again: it writes to a writer rather than to a buffer, and it
        is not what either of these wants.
*/
static p8 process_basename_storage[PATH_MAX];
static p8 process_dirname_storage[PATH_MAX];

static string_address basename(string_address path)
{
        if (is_null(path) || string_get(path) == end)
        {
                process_basename_storage[0] = '.';
                process_basename_storage[1] = end;
                return process_basename_storage;
        }

        path_tail_copy(process_basename_storage, PATH_MAX, path);

        return process_basename_storage;
}

static string_address dirname(string_address path)
{
        if (is_null(path) || string_get(path) == end)
        {
                process_dirname_storage[0] = '.';
                process_dirname_storage[1] = end;
                return process_dirname_storage;
        }

        path_head_copy(process_dirname_storage, PATH_MAX, path);

        /*
                The one place path_head_copy and glibc disagree, and POSIX is
                on glibc's side.

                XBD 4.13 says a pathname beginning with exactly two slashes
                may name something the implementation chooses, and a single
                or triple slash may not -- so "//" and "//usr" have "//" for
                their directory part where "/usr" and "///usr" have "/".
                path_head_copy answers "/" for all four, which is right for
                every caller it has in this tree and is not what a program
                that came from glibc expects.

                It is corrected here rather than in library.c because the
                assembly is shared with path handling that does not want the
                distinction, and because the correction is exactly this: a
                head of "/" out of a path whose first two bytes are slashes
                and whose third is not.
        */
        if (process_dirname_storage[0] == '/' &&
            process_dirname_storage[1] == end && path[0] == '/' &&
            path[1] == '/' && path[2] != '/')
        {
                process_dirname_storage[1] = '/';
                process_dirname_storage[2] = end;
        }

        return process_dirname_storage;
}

//      -- getopt ------------------------------------------------------------

/*
        The four globals, which are the interface as much as the function is.

        They are real symbols and not statics, because a program that parses
        options in one file and reports the bad one in another reaches for
        optopt by name, and because an object compiled against real headers
        expects to link against these.

        optind starts at one because argv[0] is the program's own name.
        opterr starts at one because the default is to complain. optopt starts
        at '?' because that is what glibc initialises it to, and a program
        that reads it before the first call sees the same thing here.
*/
string_address optarg = null;
b32 optind = 1;
b32 opterr = 1;
b32 optopt = '?';

//      Where inside the current argument the scan is. A cluster like -abc is
//      three calls into one element, and this is what remembers that.
static string_address process_getopt_place = null;

/*
        Whether getopt has ever been called, which exists to reproduce one
        piece of glibc's observable behaviour exactly.

        glibc declares optopt as '?' and then, on the first call, copies it
        back out of a private structure whose corresponding field was zero
        filled -- so a program that reads optopt before calling getopt sees
        '?' there, and one that reads it after a call that reported no error
        sees 0. POSIX says nothing about either, because optopt is only
        defined after '?' or ':' comes back. Matching both is three lines and
        makes the differential against glibc exact rather than nearly so.
*/
static bool process_getopt_started = false;

static COLD fn process_getopt_complain(string_address program, p8 letter,
                                       string_address reason)
{
        p8 shown[2] = {letter, end};

        string_format(log_error, reason, program, shown);
}

/*
        getopt, and the one place it deliberately differs from glibc.

        glibc permutes argv by default: it moves non-options to the end so
        that `prog file -v` sees -v. POSIX says the opposite -- the first
        non-option ends the options -- and this implements POSIX, because
        permuting rewrites the caller's argv and a program that reads argv
        itself afterwards would find it rearranged underneath it.

        The two ways of asking glibc for the POSIX behaviour are a leading '+'
        in the option string and POSIXLY_CORRECT in the environment, and both
        were used to compare against: the differential harness ran every case
        through glibc with POSIXLY_CORRECT set and again with a '+' in front,
        and this agrees with both on all of them. A leading '+' is accepted
        and ignored here, so the same option string works either way.

        Everything else is glibc's behaviour and was measured, not assumed:

          - a leading ':' means silence and makes a missing argument report
            ':' rather than '?'.
          - a leading '-' makes a non-option come back as return value 1 with
            optarg pointing at it, which is the GNU RETURN_IN_ORDER mode.
          - "x:" wants an argument, attached or in the next element; "x::"
            takes one only when it is attached, and leaves optarg null
            otherwise without consuming anything.
          - optind is incremented when the last character of an element is
            reached, not when the element is finished, which is what makes -a
            and -abc leave optind in the same relative place.
          - the two diagnostics are glibc's own wording, character for
            character, so a script matching on them does not have to know
            which library it is reading.

        The parse is the fourth hand-written loop's sibling and is a state
        machine over one string; string_first_of finds the character in the
        option string, which is the scan, and the rest is the state.
*/
static b32 getopt(b32 count, string_address address_to words,
                  string_address options)
{
        string_address specification;
        bool silent;
        bool in_order = false;
        p8 letter;

        if (count < 1 || is_null(words) || is_null(options))
                return -1;

        //      Every call begins with no argument recorded, so that a '?' or
        //      a -1 never leaves the previous option's argument standing
        //      where a caller would read it. glibc clears it in the same
        //      place and for the same reason.
        optarg = null;

        if (!process_getopt_started)
        {
                process_getopt_started = true;
                optopt = 0;
        }

        //      Zero is the GNU request for a full restart, and one is the
        //      ordinary one. Both begin again at the first argument.
        if (optind == 0)
        {
                optind = 1;
                process_getopt_place = null;
        }

        if (options[0] == '-')
        {
                in_order = true;
                options++;
        }
        else if (options[0] == '+')
        {
                options++;
        }

        silent = options[0] == ':';

        if (is_null(process_getopt_place) || string_get(process_getopt_place) == end)
        {
                if (optind >= count || is_null(words[optind]))
                {
                        process_getopt_place = null;
                        return -1;
                }

                if (words[optind][0] != '-' || words[optind][1] == end)
                {
                        //      A bare "-" is an operand and not an option.
                        if (!in_order)
                        {
                                process_getopt_place = null;
                                return -1;
                        }

                        optarg = words[optind++];
                        return 1;
                }

                //      "--" ends the options and is itself consumed.
                if (words[optind][1] == '-' && words[optind][2] == end)
                {
                        optind++;
                        process_getopt_place = null;
                        return -1;
                }

                process_getopt_place = words[optind] + 1;
        }

        letter = address_to process_getopt_place++;
        specification = string_first_of(options, letter);

        //      optind moves on as the last character of the element is taken,
        //      so that everything below can ask whether there is another
        //      element without also having to know where in this one it is.
        if (string_get(process_getopt_place) == end)
                optind++;

        if (letter == ':' || is_null(specification))
        {
                optopt = letter;

                if (opterr && !silent)
                        process_getopt_complain(
                            (string_address)words[0], letter,
                            (string_address) "%s: invalid option -- '%s'\n");

                return '?';
        }

        if (specification[1] != ':')
        {
                //      No argument wanted, and nothing else to do.
                return letter;
        }

        if (specification[2] == ':')
        {
                //      Optional, and only in the same element.
                if (string_get(process_getopt_place) != end)
                {
                        optarg = process_getopt_place;
                        optind++;
                }

                process_getopt_place = null;

                return letter;
        }

        if (string_get(process_getopt_place) != end)
        {
                optarg = process_getopt_place;
                optind++;
        }
        else if (optind == count)
        {
                optopt = letter;
                process_getopt_place = null;

                if (opterr && !silent)
                        process_getopt_complain(
                            (string_address)words[0], letter,
                            (string_address)
                                "%s: option requires an argument -- '%s'\n");

                return silent ? ':' : '?';
        }
        else
        {
                optarg = words[optind++];
        }

        process_getopt_place = null;

        return letter;
}

//      -- assert ------------------------------------------------------------

/*
        assert, which is one macro and one routine that never returns.

        The routine is called __assert_fail because that is the symbol every
        compiler and every real <assert.h> emits a call to, so an object built
        against glibc's headers and linked against this finds it. The
        signature is glibc's for the same reason.

        The sentence is glibc's too, word for word and punctuation for
        punctuation:

            prog: file:42: function: Assertion `x > 0' failed.

        The program's name comes from argv[0] with the directories taken off,
        which is what glibc's __progname is, and it is left out entirely when
        there is no argv -- glibc prints no bare ": " for an empty name and
        neither does this.

        It goes to descriptor two through log_error, which flushes anything
        the buffered log is holding first. That ordering is the point: a
        program that printed a line and then tripped an assertion must have
        the line appear before the diagnostic, and abort is coming
        immediately afterwards so nothing else will get the chance to flush
        it.

        NDEBUG turns the macro into nothing, and it has to be nothing that is
        still an expression -- a bare 0 would warn where an assert is used as
        a statement in a comma expression, and a do/while would not compile
        where one is used as an expression.
*/
COLD pub DEAD_END fn __assert_fail(string_address claim, string_address file,
                                  p32 line, string_address function)
{
        p8 name[PATH_MAX];
        string_address program = program_argument(0);

        name[0] = end;

        if (!is_null(program) && string_get(program) != end)
                path_tail_copy(name, PATH_MAX, program);

        if (name[0] != end)
                string_format(log_error, (string_address) "%s: ", name);

        string_format(log_error, (string_address) "%s:%p: ", is_null(file) ? (string_address) "" : file,
                      (positive)line);

        if (!is_null(function))
                string_format(log_error, (string_address) "%s: ", function);

        string_format(log_error, (string_address) "Assertion `%s' failed.\n",
                      is_null(claim) ? (string_address) "" : claim);

        abort();
}

#undef assert

#ifdef NDEBUG
#define assert(claim) ((fn)0)
#else
#define assert(claim)                                                    \
        ((claim) ? (fn)0                                                 \
                 : __assert_fail((string_address)#claim,                 \
                                 (string_address)__FILE__, (p32)__LINE__, \
                                 (string_address)__func__))
#endif

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_PROCESS
