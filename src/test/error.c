#include "../compiler_memory.c"

/*
        errno, strerror and the POSIX wrappers, against glibc.

        Two halves. The self-checking half pins the things a reference cannot
        show -- that a wrapper leaves the library's own negative-errno routine
        alone, that the message table has no accidental hole, that the stat
        layout the kernel filled is the one this thinks it is. The transcript
        half prints a stream of lines that src/test/error_reference.c, built
        against the real glibc, prints identically, so a diff of the two is
        the check. Anything printed with the TRANSCRIPT prefix is part of that
        contract and must not be reordered.

        Everything goes to descriptor one, including what perror writes,
        because descriptor two is pointed at descriptor one before anything
        runs and the buffered log is flushed before every perror. Without both
        of those the two streams interleave differently on each run and the
        diff is noise.
*/

#include "counted.inc"

#define ERROR_TEST_DIRECTORY "/tmp/dawning-error-test"
#define ERROR_TEST_FILE "/tmp/dawning-error-test/one"
#define ERROR_TEST_LINK "/tmp/dawning-error-test/link"
#define ERROR_TEST_ABSENT "/tmp/dawning-error-test/absent"

/*
        A buffer shown byte for byte, so a terminator that is or is not there
        is visible in the diff.

        The buffer is filled with '@' before the call and this prints two
        bytes past what was asked for, so a routine that wrote one byte too
        many shows an overwritten '@' rather than nothing at all. A zero byte
        prints as '|', which is what glibc's side prints for it too.
*/
static fn error_test_shown(p8 address_to into, p8 address_to buffer,
                           positive count)
{
        positive at;

        for (at = 0; at < count; at++)
                into[at] = buffer[at] == 0 ? '|' : buffer[at];

        into[count] = end;
}

static fn error_test_strerror_r_line(b32 number, positive size)
{
        p8 buffer[128];
        p8 shown[136];
        b32 answer;

        memory_fill(buffer, '@', sizeof buffer);
        answer = strerror_r(number, buffer, size);
        error_test_shown(shown, buffer, size + 2 < 128 ? size + 2 : 128);

        string_format(log, "TRANSCRIPT strerror_r %b %p rc=%b [%s]\n",
                      (bipolar)number, size, (bipolar)answer, shown);
}

static fn error_test_messages(void)
{
        b32 number;

        for (number = 0; number <= 140; number++)
                string_format(log, "TRANSCRIPT strerror %b [%s]\n",
                              (bipolar)number, strerror(number));

        string_format(log, "TRANSCRIPT strerror %b [%s]\n", (bipolar)-1,
                      strerror(-1));
        string_format(log, "TRANSCRIPT strerror %b [%s]\n", (bipolar)-7,
                      strerror(-7));
        string_format(log, "TRANSCRIPT strerror %b [%s]\n", (bipolar)1000,
                      strerror(1000));

        //      Two known messages, one long and one short, at every size
        //      around their length; then the two holes in the numbering,
        //      where EINVAL has to beat ERANGE.
        error_test_strerror_r_line(2, 64);
        error_test_strerror_r_line(2, 26);
        error_test_strerror_r_line(2, 25);
        error_test_strerror_r_line(2, 24);
        error_test_strerror_r_line(2, 10);
        error_test_strerror_r_line(2, 1);
        error_test_strerror_r_line(2, 0);
        error_test_strerror_r_line(0, 8);
        error_test_strerror_r_line(0, 7);
        error_test_strerror_r_line(84, 49);
        error_test_strerror_r_line(84, 48);
        error_test_strerror_r_line(41, 64);
        error_test_strerror_r_line(41, 10);
        error_test_strerror_r_line(41, 0);
        error_test_strerror_r_line(58, 64);
        error_test_strerror_r_line(133, 64);
        error_test_strerror_r_line(134, 64);
        error_test_strerror_r_line(-7, 64);
}

static fn error_test_perror(void)
{
        log_flush();
        errno = ENOENT;
        perror((string_address) "TRANSCRIPT perror named");

        log_flush();
        errno = ENOENT;
        perror((string_address) "");

        log_flush();
        errno = ENOENT;
        perror(null);

        log_flush();
        errno = 41;
        perror((string_address) "TRANSCRIPT perror unknown");

        log_flush();
        errno = 0;
        perror((string_address) "TRANSCRIPT perror success");
}

//      Result and errno on one line, which is the whole of what a wrapper
//      promises and the whole of what the reference prints.
static fn error_test_said(string_address what, bipolar result)
{
        string_format(log, "TRANSCRIPT call %s -> %b errno %b\n", what, result,
                      (bipolar)errno);
}

static fn error_test_wrappers(void)
{
        p8 room[64];
        p8 small[4];
        b32 pair[2];
        b32 handle;
        error_stat one;
        bipolar got;

        //      A clean slate, without reporting whether the cleaning worked:
        //      a previous run may have left nothing behind.
        unlink((string_address)ERROR_TEST_LINK);
        unlink((string_address)ERROR_TEST_FILE);
        rmdir((string_address)ERROR_TEST_DIRECTORY);

        errno = 0;
        error_test_said((string_address) "mkdir",
                        mkdir((string_address)ERROR_TEST_DIRECTORY, 0755));

        errno = 0;
        error_test_said((string_address) "mkdir-again",
                        mkdir((string_address)ERROR_TEST_DIRECTORY, 0755));

        errno = 0;
        handle = open((string_address)ERROR_TEST_FILE,
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        error_test_said((string_address) "open-create", (bipolar)(handle >= 0));

        errno = 0;
        error_test_said((string_address) "write",
                        write(handle, (address_any) "abcdefgh", 8));

        errno = 0;
        error_test_said((string_address) "lseek-end",
                        lseek(handle, 0, SEEK_END));

        errno = 0;
        error_test_said((string_address) "read-on-write-only",
                        read(handle, room, 4));

        errno = 0;
        error_test_said((string_address) "close", close(handle));

        errno = 0;
        error_test_said((string_address) "close-twice", close(handle));

        errno = 0;
        error_test_said((string_address) "open-missing",
                        open((string_address)ERROR_TEST_ABSENT, O_RDONLY));

        errno = 0;
        error_test_said((string_address) "open-exclusive-existing",
                        open((string_address)ERROR_TEST_FILE,
                             O_WRONLY | O_CREAT | O_EXCL, 0644));

        errno = 0;
        error_test_said((string_address) "open-directory-for-write",
                        open((string_address)ERROR_TEST_DIRECTORY,
                             O_WRONLY));

        errno = 0;
        error_test_said((string_address) "stat",
                        stat((string_address)ERROR_TEST_FILE, &one));

        string_format(log, "TRANSCRIPT stat size %b regular %b directory %b\n",
                      (bipolar)one.st_size,
                      (bipolar)(S_ISREG(one.st_mode) ? 1 : 0),
                      (bipolar)(S_ISDIR(one.st_mode) ? 1 : 0));

        string_format(log, "TRANSCRIPT stat permission %p links %b\n",
                      (positive)(one.st_mode & 07777),
                      (bipolar)one.st_nlink);

        errno = 0;
        error_test_said((string_address) "stat-missing",
                        stat((string_address)ERROR_TEST_ABSENT, &one));

        errno = 0;
        error_test_said((string_address) "stat-directory",
                        stat((string_address)ERROR_TEST_DIRECTORY, &one));

        string_format(log, "TRANSCRIPT stat directory-bit %b\n",
                      (bipolar)(S_ISDIR(one.st_mode) ? 1 : 0));

        errno = 0;
        error_test_said((string_address) "symlink",
                        symlink((string_address) "one",
                                (string_address)ERROR_TEST_LINK));

        errno = 0;
        memory_fill(room, '@', sizeof room);
        got = readlink((string_address)ERROR_TEST_LINK, room, sizeof room);
        error_test_said((string_address) "readlink", got);

        errno = 0;
        error_test_said((string_address) "readlink-not-a-link",
                        readlink((string_address)ERROR_TEST_FILE, room,
                                 sizeof room));

        errno = 0;
        error_test_said((string_address) "lstat-is-link",
                        lstat((string_address)ERROR_TEST_LINK, &one));

        string_format(log, "TRANSCRIPT lstat link-bit %b\n",
                      (bipolar)(S_ISLNK(one.st_mode) ? 1 : 0));

        errno = 0;
        error_test_said((string_address) "access-existing",
                        access((string_address)ERROR_TEST_FILE, F_OK));

        errno = 0;
        error_test_said((string_address) "access-missing",
                        access((string_address)ERROR_TEST_ABSENT, F_OK));

        errno = 0;
        error_test_said((string_address) "rmdir-not-empty",
                        rmdir((string_address)ERROR_TEST_DIRECTORY));

        errno = 0;
        error_test_said((string_address) "unlink-missing",
                        unlink((string_address)ERROR_TEST_ABSENT));

        errno = 0;
        error_test_said((string_address) "rename",
                        rename((string_address)ERROR_TEST_FILE,
                               (string_address)ERROR_TEST_ABSENT));

        errno = 0;
        error_test_said((string_address) "rename-back",
                        rename((string_address)ERROR_TEST_ABSENT,
                               (string_address)ERROR_TEST_FILE));

        errno = 0;
        error_test_said((string_address) "rename-missing",
                        rename((string_address)ERROR_TEST_ABSENT,
                               (string_address)ERROR_TEST_LINK));

        errno = 0;
        error_test_said((string_address) "pipe", pipe(pair));

        errno = 0;
        error_test_said((string_address) "lseek-on-pipe",
                        lseek(pair[0], 0, SEEK_SET));

        errno = 0;
        error_test_said((string_address) "isatty-on-pipe",
                        (bipolar)isatty(pair[0]));

        errno = 0;
        error_test_said((string_address) "isatty-bad-handle",
                        (bipolar)isatty(999));

        errno = 0;
        error_test_said((string_address) "dup2-onto-self",
                        dup2(pair[0], pair[0]));

        errno = 0;
        error_test_said((string_address) "dup2-closed-onto-self",
                        dup2(900, 900));

        errno = 0;
        error_test_said((string_address) "close-pipe-read", close(pair[0]));

        errno = 0;
        error_test_said((string_address) "close-pipe-write", close(pair[1]));

        errno = 0;
        error_test_said((string_address) "write-bad-handle",
                        write(999, (address_any) "x", 1));

        errno = 0;
        error_test_said((string_address) "getcwd-too-small",
                        (bipolar)(getcwd(small, sizeof small) == null));

        errno = 0;
        error_test_said((string_address) "getpid-positive",
                        (bipolar)(getpid() > 0));

        errno = 0;
        error_test_said((string_address) "chmod",
                        chmod((string_address)ERROR_TEST_FILE, 0600));

        errno = 0;
        error_test_said((string_address) "stat-after-chmod",
                        stat((string_address)ERROR_TEST_FILE, &one));

        string_format(log, "TRANSCRIPT stat permission %p\n",
                      (positive)(one.st_mode & 07777));

        errno = 0;
        error_test_said((string_address) "chmod-missing",
                        chmod((string_address)ERROR_TEST_ABSENT, 0600));

        errno = 0;
        error_test_said((string_address) "unlink-link",
                        unlink((string_address)ERROR_TEST_LINK));

        errno = 0;
        error_test_said((string_address) "unlink", 
                        unlink((string_address)ERROR_TEST_FILE));

        errno = 0;
        error_test_said((string_address) "rmdir",
                        rmdir((string_address)ERROR_TEST_DIRECTORY));
}


#define ERROR_MORE_DIRECTORY "/tmp/dawning-error-more"
#define ERROR_MORE_FILE "/tmp/dawning-error-more/two"
#define ERROR_MORE_ABSENT "/tmp/dawning-error-more/absent/deeper"

/*
        The wrappers the first walk never reached, each on the path that
        proves its arguments went to the kernel in the right order.

        An error path is the cheap way to do that and not a weak one. A
        transposed argument in a five argument call does not fail to compile
        and does not fail to run -- it produces a different errno, and a
        differential against glibc reads that errno. fchownat with its path
        and its owner swapped answers EFAULT where the right order answers
        ENOENT; an *at call handed a descriptor where the flags belong answers
        EBADF where the right order answers ENOENT. Both show up here as a
        line that differs.

        A descriptor of -1 is the shape for everything that takes one, a path
        under a directory that does not exist is the shape for everything that
        takes a path, and -1 as the directory of an *at call with a relative
        name is the shape that pins which argument is the directory.

        fork, waitpid and _exit get a real round trip instead, because a wrong
        clone flag word does not come back as an error -- it comes back as a
        child that is a thread sharing this stack, and the way that shows is
        that the parent never sees SIGCHLD and never reaps anything.
*/
static fn error_test_more(void)
{
        p8 room[64];
        b32 pair[2];
        b32 handle;
        b32 copy;
        b32 status;
        b32 child;
        p32 mask;
        error_stat one;
        struct stat tagged;
        address_any region;

        rmdir((string_address)ERROR_MORE_DIRECTORY);

        errno = 0;
        error_test_said((string_address) "more-mkdir",
                        mkdir((string_address)ERROR_MORE_DIRECTORY, 0755));

        errno = 0;
        handle = creat((string_address)ERROR_MORE_FILE, 0644);
        error_test_said((string_address) "creat", (bipolar)(handle >= 0));

        errno = 0;
        error_test_said((string_address) "creat-missing-directory",
                        creat((string_address)ERROR_MORE_ABSENT, 0644));

        errno = 0;
        error_test_said((string_address) "pwrite",
                        pwrite(handle, (address_any) "abcdefgh", 8, 0));

        errno = 0;
        memory_fill(room, '@', sizeof room);
        error_test_said((string_address) "pread",
                        pread(handle, room, 4, 2));

        room[4] = end;
        string_format(log, "TRANSCRIPT pread saw [%s]\n", room);

        errno = 0;
        error_test_said((string_address) "pread-bad-handle",
                        pread(-1, room, 4, 0));

        errno = 0;
        error_test_said((string_address) "pwrite-bad-handle",
                        pwrite(-1, (address_any) "x", 1, 0));

        errno = 0;
        error_test_said((string_address) "fstat", fstat(handle, &one));

        string_format(log, "TRANSCRIPT fstat size %b regular %b\n",
                      (bipolar)one.st_size,
                      (bipolar)(S_ISREG(one.st_mode) ? 1 : 0));

        //      The tag and the typedef are meant to be the same type, and
        //      nothing else in this file writes the tag, so it is written
        //      once here or the promise is never compiled.
        errno = 0;
        error_test_said((string_address) "fstat-through-the-tag",
                        fstat(handle, &tagged));

        string_format(log, "TRANSCRIPT tagged size %b\n",
                      (bipolar)tagged.st_size);

        errno = 0;
        error_test_said((string_address) "fstat-bad-handle", fstat(-1, &one));

        errno = 0;
        error_test_said((string_address) "fchmod", fchmod(handle, 0600));

        errno = 0;
        error_test_said((string_address) "fchmod-bad-handle",
                        fchmod(-1, 0600));

        errno = 0;
        error_test_said((string_address) "fchown-no-change",
                        fchown(handle, (p32)-1, (p32)-1));

        errno = 0;
        error_test_said((string_address) "fchown-bad-handle",
                        fchown(-1, (p32)-1, (p32)-1));

        errno = 0;
        error_test_said((string_address) "ftruncate", ftruncate(handle, 4));

        errno = 0;
        error_test_said((string_address) "ftruncate-bad-handle",
                        ftruncate(-1, 4));

        errno = 0;
        error_test_said((string_address) "fsync", fsync(handle));

        errno = 0;
        error_test_said((string_address) "fsync-bad-handle", fsync(-1));

        errno = 0;
        error_test_said((string_address) "fdatasync", fdatasync(handle));

        errno = 0;
        error_test_said((string_address) "fdatasync-bad-handle",
                        fdatasync(-1));

        errno = 0;
        error_test_said((string_address) "isatty-on-file",
                        (bipolar)isatty(handle));

        errno = 0;
        error_test_said((string_address) "fcntl-getfd",
                        fcntl(handle, 1, (positive)0));

        errno = 0;
        error_test_said((string_address) "fcntl-bad-handle",
                        fcntl(-1, 1, (positive)0));

        errno = 0;
        error_test_said((string_address) "ioctl-bad-handle",
                        ioctl(-1, 0x5401, (positive)room));

        errno = 0;
        error_test_said((string_address) "close-more", close(handle));

        //      Truncate by name, which is the one path-taking size call.
        errno = 0;
        error_test_said((string_address) "truncate",
                        truncate((string_address)ERROR_MORE_FILE, 2));

        errno = 0;
        error_test_said((string_address) "truncate-missing",
                        truncate((string_address)ERROR_MORE_ABSENT, 0));

        //      The *at forms, first with the working directory and then with
        //      a descriptor that is not one, which is what says that the
        //      first argument is the directory and not something else.
        errno = 0;
        handle = openat(AT_FDCWD, (string_address)ERROR_MORE_FILE, O_RDONLY);
        error_test_said((string_address) "openat", (bipolar)(handle >= 0));

        errno = 0;
        error_test_said((string_address) "close-openat", close(handle));

        errno = 0;
        error_test_said((string_address) "openat-bad-directory",
                        openat(-1, (string_address) "relative-name",
                               O_RDONLY));

        errno = 0;
        error_test_said((string_address) "fstatat",
                        fstatat(AT_FDCWD, (string_address)ERROR_MORE_FILE,
                                &one, 0));

        errno = 0;
        error_test_said((string_address) "fstatat-bad-directory",
                        fstatat(-1, (string_address) "relative-name", &one,
                                0));

        errno = 0;
        error_test_said((string_address) "faccessat",
                        faccessat(AT_FDCWD, (string_address)ERROR_MORE_FILE,
                                  F_OK, 0));

        errno = 0;
        error_test_said((string_address) "faccessat-bad-directory",
                        faccessat(-1, (string_address) "relative-name", F_OK,
                                  0));

        errno = 0;
        error_test_said((string_address) "mkdirat-bad-directory",
                        mkdirat(-1, (string_address) "relative-name", 0755));

        errno = 0;
        error_test_said((string_address) "unlinkat-bad-directory",
                        unlinkat(-1, (string_address) "relative-name", 0));

        errno = 0;
        error_test_said((string_address) "readlinkat-bad-directory",
                        readlinkat(-1, (string_address) "relative-name", room,
                                   sizeof room));

        errno = 0;
        error_test_said((string_address) "renameat-missing",
                        renameat2(AT_FDCWD,
                                  (string_address)ERROR_MORE_ABSENT,
                                  AT_FDCWD,
                                  (string_address)ERROR_MORE_ABSENT, 0));

        //      link and the two ownership calls by name, whose only reachable
        //      error without being root is a path that is not there.
        errno = 0;
        error_test_said((string_address) "link-missing",
                        link((string_address)ERROR_MORE_ABSENT,
                             (string_address)ERROR_MORE_ABSENT));

        errno = 0;
        error_test_said((string_address) "link",
                        link((string_address)ERROR_MORE_FILE,
                             (string_address) "/tmp/dawning-error-more/three"));

        errno = 0;
        error_test_said((string_address) "unlink-link-two",
                        unlink((string_address) "/tmp/dawning-error-more/three"));

        errno = 0;
        error_test_said((string_address) "chown-no-change",
                        chown((string_address)ERROR_MORE_FILE, (p32)-1,
                              (p32)-1));

        errno = 0;
        error_test_said((string_address) "chown-missing",
                        chown((string_address)ERROR_MORE_ABSENT, (p32)-1,
                              (p32)-1));

        errno = 0;
        error_test_said((string_address) "lchown-no-change",
                        lchown((string_address)ERROR_MORE_FILE, (p32)-1,
                               (p32)-1));

        errno = 0;
        error_test_said((string_address) "lchown-missing",
                        lchown((string_address)ERROR_MORE_ABSENT, (p32)-1,
                               (p32)-1));

        //      Descriptors: dup, dup3 and pipe2, and the equal-handle case
        //      dup2 has to keep away from dup3.
        errno = 0;
        copy = dup(1);
        error_test_said((string_address) "dup", (bipolar)(copy >= 3));

        errno = 0;
        error_test_said((string_address) "close-dup", close(copy));

        errno = 0;
        error_test_said((string_address) "dup-bad-handle", dup(-1));

        errno = 0;
        error_test_said((string_address) "dup3", dup3(1, 7, 0));

        errno = 0;
        error_test_said((string_address) "close-dup3", close(7));

        errno = 0;
        error_test_said((string_address) "dup3-bad-handle", dup3(-1, 7, 0));

        errno = 0;
        error_test_said((string_address) "pipe2", pipe2(pair, 0));

        errno = 0;
        error_test_said((string_address) "close-pipe2-read", close(pair[0]));

        errno = 0;
        error_test_said((string_address) "close-pipe2-write", close(pair[1]));

        //      chdir and fchdir, put back immediately so nothing after this
        //      depends on where it is standing.
        errno = 0;
        error_test_said((string_address) "chdir",
                        chdir((string_address)ERROR_MORE_DIRECTORY));

        errno = 0;
        error_test_said((string_address) "chdir-back",
                        chdir((string_address) "/tmp"));

        errno = 0;
        error_test_said((string_address) "chdir-missing",
                        chdir((string_address)ERROR_MORE_ABSENT));

        errno = 0;
        error_test_said((string_address) "fchdir-bad-handle", fchdir(-1));

        //      umask, which cannot fail and is checked by what it gives back.
        umask(0022);
        mask = umask(0077);
        string_format(log, "TRANSCRIPT umask previous %p\n", (positive)mask);
        umask(mask);

        //      The mapping calls, whose failure is a pointer and not a minus
        //      one, so they are the only users of the third translation.
        errno = 0;
        region = mmap(null, 4096, FILE_PROTECT_READ | FILE_PROTECT_WRITE,
                      FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS, -1, 0);
        error_test_said((string_address) "mmap",
                        (bipolar)(region != address_bad));

        //      Written to before it is unmapped, because a mapping that is
        //      not really there fails here rather than in the munmap.
        memory_fill(region, 0x5a, 4096);

        errno = 0;
        error_test_said((string_address) "mprotect",
                        mprotect(region, 4096, FILE_PROTECT_READ));

        errno = 0;
        error_test_said((string_address) "munmap", munmap(region, 4096));

        errno = 0;
        error_test_said((string_address) "mmap-zero-length",
                        (bipolar)(mmap(null, 0, FILE_PROTECT_READ,
                                       FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS,
                                       -1, 0) == address_bad));

        errno = 0;
        error_test_said((string_address) "munmap-zero-length",
                        munmap(null, 0));

        //      The identity calls, which cannot fail and are compared as
        //      values because both programs run as the same user.
        string_format(log, "TRANSCRIPT identity ppid-positive %b uid-is-euid %b"
                           " gid-is-egid %b\n",
                      (bipolar)(getppid() > 0),
                      (bipolar)(getuid() == geteuid()),
                      (bipolar)(getgid() == getegid()));

        errno = 0;
        error_test_said((string_address) "kill-probe-self",
                        kill(getpid(), 0));

        errno = 0;
        error_test_said((string_address) "kill-bad-signal", kill(getpid(),
                                                                9999));

        errno = 0;
        error_test_said((string_address) "execve-missing",
                        execve((string_address) "/nonexistent-dawning-execve",
                               null, null));

        errno = 0;
        error_test_said((string_address) "sync", sync());

        /*
                A child, and the three calls that only mean anything together.

                The log is flushed first because the child inherits whatever
                is still in the buffer, and although _exit does not flush it,
                a child that ever did would print this run's output twice.
        */
        log_flush();

        errno = 0;
        child = fork();

        if (child == 0)
                _exit(7);

        status = 0;
        errno = 0;
        error_test_said((string_address) "fork-then-waitpid",
                        (bipolar)(waitpid(child, &status, 0) == child));

        string_format(log, "TRANSCRIPT child status %b\n", (bipolar)status);

        log_flush();

        errno = 0;
        child = fork();

        if (child == 0)
                _Exit(3);

        status = 0;
        errno = 0;
        error_test_said((string_address) "fork-then-wait",
                        (bipolar)(wait(&status) == child));

        string_format(log, "TRANSCRIPT second child status %b\n",
                      (bipolar)status);

        errno = 0;
        error_test_said((string_address) "waitpid-no-children",
                        waitpid(-1, &status, 0));

        errno = 0;
        error_test_said((string_address) "wait4-no-children",
                        wait4(-1, &status, 0, null));

        errno = 0;
        error_test_said((string_address) "more-unlink",
                        unlink((string_address)ERROR_MORE_FILE));

        errno = 0;
        error_test_said((string_address) "more-rmdir",
                        rmdir((string_address)ERROR_MORE_DIRECTORY));
}

/*
        What a reference cannot show.

        The point of the whole design is that the library's own routines did
        not change, so the first group calls a library routine and a wrapper
        against the same missing file and requires the two different answers.
        The rest are structural: the table has an entry wherever Linux has a
        number, the two holes are holes, and the structure the kernel filled
        has its fields where this file says they are.
*/
static fn error_test_contract(void)
{
        file one;
        error_stat filled;
        b32 number;
        b32 handle;
        positive length;
        p8 room[ERROR_MESSAGE_MAX];

        errno = 0;
        file_new(&one, (string_address)"/nonexistent-dawning-error", FILE_READ);
        check("library routine still returns the kernel number",
              (bipolar)one.handle == -ENOENT);
        check("library routine left errno alone", errno == 0);

        errno = 0;
        handle = open((string_address) "/nonexistent-dawning-error", O_RDONLY);
        check("wrapper returned minus one", handle == -1);
        check("wrapper set errno", errno == ENOENT);

        //      A second failing call overwrites the first, which is the whole
        //      of what is wrong with errno and is worth having pinned.
        errno = 0;
        close(-1);
        check("second failure overwrote the first", errno == EBADF);

        //      A succeeding call must not clear it. Programs depend on this:
        //      the idiom is to zero errno, do the work, then look.
        errno = EPIPE;
        getpid();
        check("success left errno untouched", errno == EPIPE);

        check("errno is an lvalue through the accessor",
              address_of errno == __errno_location());

        for (number = 0; number <= ERROR_HIGHEST; number++)
        {
                if (number == 41 || number == 58)
                        continue;

                checks++;
                if (is_null(error_messages[number]))
                {
                        failures++;
                        string_format(log, "  FAIL message missing for %b\n",
                                      (bipolar)number);
                }
        }

        check("41 is a hole", is_null(error_messages[41]));
        check("58 is a hole", is_null(error_messages[58]));

        //      The two spellings that are one number, which a program may
        //      compare either way.
        check("EWOULDBLOCK is EAGAIN", EWOULDBLOCK == EAGAIN);
        check("ENOTSUP is EOPNOTSUPP", ENOTSUP == EOPNOTSUPP);
        check("EDEADLOCK is EDEADLK", EDEADLOCK == EDEADLK);

        //      The layout, checked against a structure the kernel actually
        //      filled rather than against arithmetic done here. /dev/null is a
        //      character device of size zero on every Linux there is.
        memory_fill(&filled, 0xa5, sizeof filled);
        check("stat of /dev/null",
              stat((string_address) "/dev/null", &filled) == 0);
        check("mode says character device", S_ISCHR(filled.st_mode));
        check("size is zero", filled.st_size == 0);
        check("link count is one", filled.st_nlink == 1);
        check("permissions are readable and writable by all",
              (filled.st_mode & 0666) == 0666);

#if X64
        check("structure is the x86_64 size", sizeof(error_stat) == 144);
#else
        check("structure is the asm-generic size", sizeof(error_stat) == 128);
#endif

        //      The GNU extension this deliberately does not have. glibc
        //      allocates a buffer for a null pointer; this refuses, so the
        //      answer cannot be compared against the reference and is pinned
        //      here instead.
        errno = 0;
        check("getcwd of a null buffer is refused", is_null(getcwd(null, 64)));
        check("and refused with EINVAL", errno == EINVAL);

        /*
                strerror_r terminates what it wrote at every size above zero,
                and touches nothing past the size it was given.

                84 is the longest message in the table, forty nine bytes, so
                the loop below crosses both regimes: under fifty the message
                is cut and the terminator lands at the last byte of the
                buffer, at fifty and above the whole message fits and the
                terminator lands right after it with the rest untouched.
        */
        length = string_length(error_messages[84]);

        for (number = 1; number < (b32)sizeof room; number++)
        {
                positive size = (positive)number;
                positive at = length < size ? length : size - 1;

                memory_fill(room, '@', sizeof room);
                strerror_r(84, room, size);

                checks++;
                if (room[at] != end || (at + 1 < sizeof room
                                        && room[at + 1] != '@'))
                {
                        failures++;
                        string_format(log,
                                      "  FAIL terminator wrong at size %b\n",
                                      (bipolar)number);
                }
        }

        //      And must write nothing at all at size zero.
        memory_fill(room, '@', sizeof room);
        strerror_r(84, room, 0);
        check("size zero wrote nothing", room[0] == '@');

        //      Nothing in the table may outgrow the buffer perror builds its
        //      line in, which is the assumption that would fail silently.
        for (number = 0; number <= ERROR_HIGHEST; number++)
        {
                if (is_null(error_messages[number]))
                        continue;

                checks++;
                if (string_length(error_messages[number])
                    >= ERROR_MESSAGE_MAX)
                {
                        failures++;
                        string_format(log, "  FAIL message %b is too long\n",
                                      (bipolar)number);
                }
        }

        check("strerror of a known number is the table entry",
              strerror(ENOENT) == error_messages[ENOENT]);

        /*
                The three that the differential cannot reach, checked here
                instead of against glibc.

                getdents64 is behind _GNU_SOURCE in glibc's headers and
                _GNU_SOURCE would swap strerror_r for the other one, so the
                reference cannot call it. setsid succeeds by detaching the
                caller from its controlling terminal, which is not a thing to
                do to a test process, so it is done in a child. And wait4's
                fourth argument is a buffer the kernel writes, which the
                transcript only ever passes as null -- so nothing so far says
                that argument is in the right place.
        */
        handle = open((string_address) "/tmp", O_RDONLY | O_DIRECTORY);
        check("directory opened for reading", handle >= 0);

        {
                p8 entries[1024];
                bipolar got;

                errno = 0;
                got = getdents64(handle, entries, sizeof entries);
                check("getdents64 returned entries", got > 0);
                check("getdents64 left errno alone", errno == 0);

                errno = 0;
                check("getdents64 on a bad handle",
                      getdents64(-1, entries, sizeof entries) == -1);
                check("and reported EBADF", errno == EBADF);
        }

        check("directory closed", close(handle) == 0);

        {
                b32 child = fork();
                b32 status = 0;

                if (child == 0)
                        _exit(setsid() < 0 ? 1 : 0);

                check("child reaped", waitpid(child, &status, 0) == child);
                check("setsid succeeded in the child", status == 0);
        }

        {
                //      struct rusage is 144 bytes on all three here, and the
                //      only thing this needs to know is that the kernel wrote
                //      into it -- a fourth argument in the wrong place either
                //      faults or leaves the fill byte alone.
                p8 usage[144];
                b32 child;
                b32 status = 0;

                memory_fill(usage, 0xa5, sizeof usage);

                child = fork();

                if (child == 0)
                        _exit(0);

                check("wait4 reaped the child",
                      wait4(child, &status, 0, usage) == child);
                check("wait4 filled the usage buffer",
                      !(usage[0] == 0xa5 && usage[8] == 0xa5
                        && usage[16] == 0xa5));
        }
}

b32 main(void)
{
        //      Both halves of the transcript have to arrive on one descriptor
        //      in one order, and perror writes to two.
        dup2(1, 2);

        error_test_messages();
        error_test_perror();
        error_test_wrappers();
        error_test_more();
        error_test_contract();

        return test_report(null);
}
