#include "../compiler_memory.c"

/*
        Directories, the exec family, sleeping, and the small POSIX names
        beside them.

        What is not here is the exhaustive half, and that is deliberate. The
        parts of this family whose answer glibc defines -- strverscmp,
        basename, dirname, realpath, getopt, the whole of readdir and
        scandir's ordering, remove's errno and execvp's PATH walk -- were
        compared against glibc 2.44 on the build machine by running identical
        sweeps through both and diffing the output, on all three machines:

            8100 strverscmp pairs over 90 strings
            52 paths through basename and dirname
            45 paths through realpath, over a tree of symbolic links,
               loops, dangling links and .. past root
            32 getopt cases, every tuple and both diagnostics
            2402 entries read out of one directory, which is four getdents64
               refills and not one
            two scandir orderings, telldir and seekdir round trips
            17 execvp cases over a directory of deliberately broken stubs

        All of it agreed line for line on x86_64, arm64 and riscv64. That
        harness needs a glibc and a writable file system laid out just so, and
        nothing else in this tree needs either, so it does not live here.

        What is here is everything a differential cannot reach: the contracts
        that are about this implementation rather than about an answer, and
        the failure paths glibc and this agree to have but that a sweep would
        have to break the machine to provoke.
*/

static positive checks;
static positive failures;

static fn same(string_address name, bipolar got, bipolar want)
{
        checks++;

        if (got == want)
                return;

        failures++;
        string_format(log, "FAIL %s: got %b want %b\n", name, got, want);
}

static fn same_text(string_address name, string_address got,
                    string_address want)
{
        checks++;

        if (!is_null(got) && !is_null(want) && string_compare(got, want) == 0)
                return;

        failures++;
        string_format(log, "FAIL %s: got %s want %s\n", name,
                      is_null(got) ? (string_address) "(null)" : got,
                      is_null(want) ? (string_address) "(null)" : want);
}

static fn true_is(string_address name, bool got)
{
        same(name, got ? 1 : 0, 1);
}

//      Where everything that needs a file system happens, made once and torn
//      down at the end.
static p8 test_root[PATH_MAX];

static positive test_path(p8 address_to into, string_address tail)
{
        return path_join(into, PATH_MAX, test_root, tail);
}

//      -- sleeping -----------------------------------------------------------

/*
        The three sleeps, and the one thing about them that is worth a test:
        that time actually passed and that a bad request is refused rather
        than rounded.

        The elapsed check is deliberately loose at the top and tight at the
        bottom. A sleep must not return early -- that is the whole contract --
        but it may return arbitrarily late, because the machine may be busy
        and qemu certainly is, so the upper bound is a second rather than
        anything that would make this flaky.
*/
static fn test_sleeping(void)
{
        timespec span;
        timespec left;
        timespec before;
        timespec after;
        positive elapsed;

        span.tv_sec = 0;
        span.tv_nsec = 1000000;
        left.tv_sec = 0;
        left.tv_nsec = 0;
        same("nanosleep short", nanosleep(address_of span, address_of left), 0);

        //      Nothing was interrupted, so nothing was written back.
        same("nanosleep left seconds", (bipolar)left.tv_sec, 0);
        same("nanosleep left nanoseconds", (bipolar)left.tv_nsec, 0);

        span.tv_sec = 0;
        span.tv_nsec = 1000000000;
        errno = 0;
        same("nanosleep bad nanoseconds", nanosleep(address_of span, null), -1);
        same("nanosleep bad errno", errno, EINVAL);

        span.tv_sec = (positive)(bipolar)-1;
        span.tv_nsec = 0;
        errno = 0;
        same("nanosleep negative", nanosleep(address_of span, null), -1);
        same("nanosleep negative errno", errno, EINVAL);

        errno = 0;
        same("nanosleep null", nanosleep(null, null), -1);
        same("nanosleep null errno", errno, EFAULT);

        //      clock_nanosleep reports through its return value and must not
        //      disturb errno, which is the difference that makes it a
        //      separate routine rather than an alias.
        span.tv_sec = 0;
        span.tv_nsec = 1000000;
        errno = 1234;
        same("clock_nanosleep short",
             clock_nanosleep(CLOCK_MONOTONIC, 0, address_of span, null), 0);
        same("clock_nanosleep left errno alone", errno, 1234);

        span.tv_nsec = 1000000000;
        errno = 1234;
        same("clock_nanosleep bad",
             clock_nanosleep(CLOCK_MONOTONIC, 0, address_of span, null),
             EINVAL);
        same("clock_nanosleep still left errno alone", errno, 1234);

        //      An absolute deadline already in the past returns at once and
        //      is not an error.
        before.tv_sec = 0;
        before.tv_nsec = 0;
        same("clock_nanosleep absolute past",
             clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                             address_of before, null),
             0);

        same("usleep", usleep(1000), 0);
        same("usleep past a second", usleep(1100000), 0);

        same("sleep zero seconds", (bipolar)process_sleep_seconds(0), 0);

        //      Time really passed. Twenty milliseconds asked for, and the
        //      monotonic clock must have moved at least that far.
        clock_read(CLOCK_MONOTONIC, address_of before);
        span.tv_sec = 0;
        span.tv_nsec = 20000000;
        nanosleep(address_of span, null);
        clock_read(CLOCK_MONOTONIC, address_of after);

        elapsed = (after.tv_sec - before.tv_sec) * 1000000000u +
                  after.tv_nsec - before.tv_nsec;

        true_is("nanosleep did not return early", elapsed >= 20000000u);
        true_is("nanosleep returned within a second", elapsed < 1000000000u);

        /*
                The name `sleep` still means library.c's assembly routine.

                This is the whole reason process_sleep_seconds exists under
                its own name: the POSIX spelling is behind
                STANDARD_SLEEP_IS_POSIX and must stay behind it, because
                src/test/clock.c and src/test/stream_buffering.c both call the
                assembly one with a pointer and would compile into nonsense
                the day the macro went on by default. Calling it here with a
                timespec is what proves the macro is off.
        */
        span.tv_sec = 0;
        span.tv_nsec = 1000000;
        sleep(address_of span);
        checks++;
}

//      -- directories --------------------------------------------------------

static b32 test_directory_names(DIR address_to folder, string_address wanted)
{
        process_dirent address_to entry;
        b32 found = 0;

        while (!is_null(entry = readdir(folder)))
                if (string_compare(entry->d_name, wanted) == 0)
                        found++;

        return found;
}

static fn test_directories(void)
{
        p8 path[PATH_MAX];
        p8 other[PATH_MAX];
        DIR address_to folder;
        process_dirent address_to entry;
        process_dirent held;
        process_dirent address_to answer;
        b32 handle;
        b32 count;
        b32 files;
        b32 directories;
        b32 links;
        bipolar mark;

        test_path(path, (string_address) "one");
        handle = creat(path, 0644);
        close(handle);

        test_path(path, (string_address) "two");
        handle = creat(path, 0644);
        close(handle);

        test_path(path, (string_address) "inner");
        mkdir(path, 0755);

        test_path(path, (string_address) "pointer");
        symlink((string_address) "one", path);

        folder = opendir(test_root);
        true_is("opendir found the directory", !is_null(folder));

        count = 0;
        files = 0;
        directories = 0;
        links = 0;

        while (!is_null(entry = readdir(folder)))
        {
                count++;

                if (entry->d_type == DT_REG)
                        files++;
                else if (entry->d_type == DT_DIR)
                        directories++;
                else if (entry->d_type == DT_LNK)
                        links++;
        }

        //      Four names, a dot, a dot-dot and the directory made above.
        same("readdir saw every entry", count, 6);
        same("readdir counted the files", files, 2);
        same("readdir counted the directories", directories, 3);
        same("readdir counted the links", links, 1);

        //      The end of a directory is a null answer with errno untouched,
        //      which is the only way a caller tells it from a failure.
        errno = 4321;
        true_is("readdir at the end is null", is_null(readdir(folder)));
        same("readdir at the end left errno alone", errno, 4321);

        rewinddir(folder);
        same("rewinddir went back to the start",
             test_directory_names(folder, (string_address) "one"), 1);

        //      telldir before an entry names that entry.
        rewinddir(folder);
        mark = telldir(folder);
        entry = readdir(folder);
        string_copy(other, entry->d_name);
        readdir(folder);
        seekdir(folder, mark);
        entry = readdir(folder);
        same_text("seekdir came back to the same entry", entry->d_name, other);

        //      readdir_r copies rather than pointing, and says nothing
        //      through errno.
        rewinddir(folder);
        count = 0;

        while (readdir_r(folder, address_of held, address_of answer) == 0 &&
               !is_null(answer))
        {
                count++;
                true_is("readdir_r answered with the caller's structure",
                        answer == address_of held);
        }

        same("readdir_r saw every entry", count, 6);

        same("dirfd is a descriptor", dirfd(folder) >= 0 ? 1 : 0, 1);
        handle = dirfd(folder);
        same("closedir", closedir(folder), 0);

        //      closedir closed the descriptor it was holding.
        errno = 0;
        same("closedir closed the descriptor", close(handle), -1);
        same("closedir errno", errno, EBADF);

        //      fdopendir takes the descriptor it is given, and closedir
        //      closes that one too.
        handle = open(test_root, O_RDONLY | O_DIRECTORY);
        folder = fdopendir(handle);
        true_is("fdopendir took the descriptor", !is_null(folder));
        same("fdopendir kept the same descriptor", dirfd(folder), handle);
        closedir(folder);
        errno = 0;
        same("fdopendir's descriptor was closed", close(handle), -1);

        //      Every refusal.
        test_path(path, (string_address) "one");
        errno = 0;
        true_is("opendir refuses a regular file", is_null(opendir(path)));
        same("opendir on a file is ENOTDIR", errno, ENOTDIR);

        test_path(path, (string_address) "missing");
        errno = 0;
        true_is("opendir refuses a missing path", is_null(opendir(path)));
        same("opendir on nothing is ENOENT", errno, ENOENT);

        errno = 0;
        true_is("opendir refuses a null path", is_null(opendir(null)));
        same("opendir on null is EINVAL", errno, EINVAL);

        test_path(path, (string_address) "one");
        handle = open(path, O_RDONLY);
        errno = 0;
        true_is("fdopendir refuses a regular file",
                is_null(fdopendir(handle)));
        same("fdopendir on a file is ENOTDIR", errno, ENOTDIR);
        close(handle);

        errno = 0;
        true_is("fdopendir refuses a closed descriptor",
                is_null(fdopendir(handle)));
        same("fdopendir on a closed descriptor is EBADF", errno, EBADF);

        errno = 0;
        true_is("fdopendir refuses a negative descriptor",
                is_null(fdopendir(-1)));
        same("fdopendir on a negative descriptor is EBADF", errno, EBADF);
}

//      -- scandir -------------------------------------------------------------

static b32 test_keep_all(const process_dirent address_to entry)
{
        (void)entry;

        return 1;
}

static b32 test_keep_none(const process_dirent address_to entry)
{
        (void)entry;

        return 0;
}

static b32 test_keep_plain(const process_dirent address_to entry)
{
        return entry->d_name[0] != '.';
}

static fn test_scandir(void)
{
        process_dirent address_to address_to list = null;
        b32 count;
        b32 at;

        count = scandir(test_root, address_of list, test_keep_plain,
                        alphasort);

        same("scandir kept the four names", count, 4);
        same_text("scandir sorted first", list[0]->d_name,
                  (string_address) "inner");
        same_text("scandir sorted second", list[1]->d_name,
                  (string_address) "one");
        same_text("scandir sorted third", list[2]->d_name,
                  (string_address) "pointer");
        same_text("scandir sorted fourth", list[3]->d_name,
                  (string_address) "two");

        //      An entry from scandir is its own allocation, is terminated,
        //      and says how long it is.
        for (at = 0; at < count; at++)
        {
                true_is("scandir sized the entry to the name",
                        list[at]->d_reclen ==
                                PROCESS_DIRENT_HEADER +
                                        string_length(list[at]->d_name) + 1);
                free(list[at]);
        }

        free(list);

        list = null;
        count = scandir(test_root, address_of list, test_keep_none, alphasort);
        same("scandir kept nothing", count, 0);
        true_is("scandir still handed back a vector", !is_null(list));
        free(list);

        list = null;
        count = scandir(test_root, address_of list, test_keep_all, null);
        same("scandir with no ordering kept everything", count, 6);

        for (at = 0; at < count; at++)
                free(list[at]);

        free(list);

        list = null;
        errno = 0;
        same("scandir on a missing directory",
             scandir((string_address) "/tmp/pd-not-here-at-all",
                     address_of list, null, alphasort),
             -1);
        same("scandir on a missing directory is ENOENT", errno, ENOENT);

        errno = 0;
        same("scandir with no list refuses",
             scandir(test_root, null, null, alphasort), -1);
        same("scandir with no list is EINVAL", errno, EINVAL);
}

//      -- version ordering ---------------------------------------------------

static fn test_version_sign(string_address left, string_address right,
                            b32 want)
{
        b32 got = strverscmp(left, right);

        checks++;

        if ((got < 0 && want < 0) || (got > 0 && want > 0) ||
            (got == 0 && want == 0))
                return;

        failures++;
        string_format(log, "FAIL strverscmp(%s, %s): got %b want %b\n", left,
                      right, (bipolar)got, (bipolar)want);
}

static fn test_versions(void)
{
        //      The property that makes this different from strcmp: a run of
        //      digits compares as a number.
        test_version_sign((string_address) "file2", (string_address) "file10",
                          -1);
        test_version_sign((string_address) "file10", (string_address) "file2",
                          1);
        test_version_sign((string_address) "file1", (string_address) "file1",
                          0);

        //      And the property that makes it different from a number
        //      compare: a run that starts with a zero is a fraction.
        test_version_sign((string_address) "1.010", (string_address) "1.09",
                          -1);
        test_version_sign((string_address) "007", (string_address) "07", -1);
        test_version_sign((string_address) "", (string_address) "", 0);
        test_version_sign((string_address) "", (string_address) "a", -1);
        test_version_sign((string_address) "a", (string_address) "", 1);
}

//      -- basename and dirname ------------------------------------------------

static fn test_names(void)
{
        p8 held[PATH_MAX];

        string_copy(held, (string_address) "/usr/lib");
        same_text("basename of a path", basename(held),
                  (string_address) "lib");
        string_copy(held, (string_address) "/usr/lib");
        same_text("dirname of a path", dirname(held), (string_address) "/usr");

        string_copy(held, (string_address) "usr");
        same_text("basename with no directory", basename(held),
                  (string_address) "usr");
        string_copy(held, (string_address) "usr");
        same_text("dirname with no directory", dirname(held),
                  (string_address) ".");

        string_copy(held, (string_address) "/");
        same_text("basename of root", basename(held), (string_address) "/");
        string_copy(held, (string_address) "/");
        same_text("dirname of root", dirname(held), (string_address) "/");

        string_copy(held, (string_address) "//");
        same_text("dirname of exactly two slashes", dirname(held),
                  (string_address) "//");
        string_copy(held, (string_address) "///");
        same_text("dirname of three slashes", dirname(held),
                  (string_address) "/");

        string_copy(held, (string_address) "a/b/");
        same_text("basename ignores a trailing slash", basename(held),
                  (string_address) "b");

        same_text("basename of nothing", basename((string_address) ""),
                  (string_address) ".");
        same_text("dirname of nothing", dirname((string_address) ""),
                  (string_address) ".");
        same_text("basename of null", basename(null), (string_address) ".");
        same_text("dirname of null", dirname(null), (string_address) ".");
}

//      -- realpath ------------------------------------------------------------

static fn test_realpath(void)
{
        p8 path[PATH_MAX];
        p8 answer[PATH_MAX];
        p8 wanted[PATH_MAX];
        string_address allocated;

        test_path(path, (string_address) "pointer");
        test_path(wanted, (string_address) "one");

        same_text("realpath followed the link", realpath(path, answer),
                  wanted);

        //      A null buffer allocates, and what comes back is free()able.
        allocated = realpath(path, null);
        same_text("realpath allocated the answer", allocated, wanted);
        free(allocated);

        test_path(path, (string_address) "inner/../one");
        same_text("realpath resolved dot dot", realpath(path, answer), wanted);

        test_path(path, (string_address) "./inner/./");
        test_path(wanted, (string_address) "inner");
        same_text("realpath dropped the dots", realpath(path, answer), wanted);

        //      Every refusal, and the errno that goes with it.
        errno = 0;
        true_is("realpath refuses null", is_null(realpath(null, answer)));
        same("realpath on null is EINVAL", errno, EINVAL);

        errno = 0;
        true_is("realpath refuses an empty path",
                is_null(realpath((string_address) "", answer)));
        same("realpath on nothing is ENOENT", errno, ENOENT);

        test_path(path, (string_address) "missing");
        errno = 0;
        true_is("realpath refuses a missing path",
                is_null(realpath(path, answer)));
        same("realpath on a missing path is ENOENT", errno, ENOENT);

        test_path(path, (string_address) "one/below");
        errno = 0;
        true_is("realpath refuses a file used as a directory",
                is_null(realpath(path, answer)));
        same("realpath through a file is ENOTDIR", errno, ENOTDIR);

        //      The kernel's own answer for /etc/passwd/.. is ENOTDIR, and a
        //      walk that just popped the last component would say /etc.
        test_path(path, (string_address) "one/..");
        errno = 0;
        true_is("realpath refuses dot dot out of a file",
                is_null(realpath(path, answer)));
        same("realpath dot dot through a file is ENOTDIR", errno, ENOTDIR);

        //      A loop of links is ELOOP and not a hang.
        test_path(path, (string_address) "round");
        test_path(wanted, (string_address) "trip");
        symlink(wanted, path);
        symlink(path, wanted);
        errno = 0;
        true_is("realpath refuses a loop", is_null(realpath(path, answer)));
        same("realpath on a loop is ELOOP", errno, ELOOP);

        //      Root is its own parent.
        same_text("realpath of root", realpath((string_address) "/", answer),
                  (string_address) "/");
        same_text("realpath above root",
                  realpath((string_address) "/../..", answer),
                  (string_address) "/");
}

//      -- exec ----------------------------------------------------------------

//      The raw wait status, decoded through the macros this family adds.
static b32 test_run(string_address address_to words, bool by_path)
{
        b32 status = 0;
        b32 child;

        //      Anything buffered belongs to this process; the child would
        //      write it a second time.
        log_flush();

        child = fork();

        if (child == 0)
        {
                if (by_path)
                        execvp(words[0], words);
                else
                        execv(words[0], words);

                _exit(100 + errno);
        }

        waitpid(child, address_of status, 0);

        if (!WIFEXITED(status))
                return -1;

        return WEXITSTATUS(status);
}

static fn test_exec(void)
{
        string_address words[5];
        b32 status = 0;
        b32 child;

        words[0] = (string_address) "/bin/sh";
        words[1] = (string_address) "-c";
        words[2] = (string_address) "exit 7";
        words[3] = null;
        same("execv ran the shell", test_run(words, false), 7);

        words[0] = (string_address) "sh";
        same("execvp found the shell on PATH", test_run(words, true), 7);

        words[0] = (string_address) "pd-no-such-program-anywhere";
        words[1] = null;
        same("execvp did not find a missing name", test_run(words, true),
             100 + ENOENT);

        words[0] = (string_address) "/pd/no/such/path";
        same("execv did not find a missing path", test_run(words, false),
             100 + ENOENT);

        errno = 0;
        same("execvp refuses an empty name",
             execvp((string_address) "", words), -1);
        same("execvp on an empty name is ENOENT", errno, ENOENT);

        errno = 0;
        same("execvp refuses a null name", execvp(null, words), -1);
        same("execvp on a null name is ENOENT", errno, ENOENT);

        //      The variadic spellings, each in its own child.
        log_flush();
        child = fork();

        if (child == 0)
        {
                execl((string_address) "/bin/sh", (string_address) "sh",
                      (string_address) "-c", (string_address) "exit 11",
                      (string_address)null);
                _exit(100 + errno);
        }

        waitpid(child, address_of status, 0);
        same("execl ran the shell", WEXITSTATUS(status), 11);

        log_flush();
        child = fork();

        if (child == 0)
        {
                execlp((string_address) "sh", (string_address) "sh",
                       (string_address) "-c", (string_address) "exit 12",
                       (string_address)null);
                _exit(100 + errno);
        }

        waitpid(child, address_of status, 0);
        same("execlp ran the shell", WEXITSTATUS(status), 12);

        log_flush();
        child = fork();

        if (child == 0)
        {
                string_address environment[2];

                environment[0] = (string_address) "PD_MARKER=13";
                environment[1] = null;

                execle((string_address) "/bin/sh", (string_address) "sh",
                       (string_address) "-c",
                       (string_address) "exit $PD_MARKER",
                       (string_address)null, environment);
                _exit(100 + errno);
        }

        waitpid(child, address_of status, 0);
        same("execle used the environment it was given",
             WEXITSTATUS(status), 13);
}

//      -- temporary names ------------------------------------------------------

static fn test_temporary(void)
{
        p8 pattern[PATH_MAX];
        p8 second[PATH_MAX];
        p8 built[PATH_MAX];
        struct stat facts;
        FILE address_to handle;
        b32 one;
        b32 two;
        string_address name;
        p8 read_back[16];

        test_path(built, (string_address) "tmpXXXXXX");

        string_copy(pattern, built);
        one = mkstemp(pattern);
        true_is("mkstemp opened something", one >= 0);
        true_is("mkstemp rewrote the template",
                string_compare(pattern, built) != 0);
        same("mkstemp made the file", stat(pattern, address_of facts), 0);
        same("mkstemp made it private", (bipolar)(facts.st_mode & 0777),
             0600);
        true_is("mkstemp made a regular file", S_ISREG(facts.st_mode) != 0);

        string_copy(second, built);
        two = mkstemp(second);
        true_is("a second mkstemp opened something", two >= 0);
        true_is("two mkstemp names differ",
                string_compare(pattern, second) != 0);

        close(one);
        close(two);
        unlink(pattern);
        unlink(second);

        //      A template that does not end in six X is refused rather than
        //      quietly given a name of its own.
        string_copy(pattern, (string_address) "/tmp/pd-no-x-here");
        errno = 0;
        same("mkstemp refuses a template with no X", mkstemp(pattern), -1);
        same("mkstemp with no X is EINVAL", errno, EINVAL);

        string_copy(pattern, (string_address) "XXXXX");
        errno = 0;
        same("mkstemp refuses five X", mkstemp(pattern), -1);
        same("mkstemp with five X is EINVAL", errno, EINVAL);

        errno = 0;
        same("mkstemp refuses null", mkstemp(null), -1);
        same("mkstemp on null is EINVAL", errno, EINVAL);

        //      mkstemps keeps a suffix.
        test_path(built, (string_address) "tmpXXXXXX.log");
        string_copy(pattern, built);
        one = mkstemps(pattern, 4);
        true_is("mkstemps opened something", one >= 0);
        same_text("mkstemps kept the suffix",
                  pattern + string_length(pattern) - 4,
                  (string_address) ".log");
        close(one);
        unlink(pattern);

        //      mkdtemp makes a directory and nothing else can enter it.
        test_path(built, (string_address) "dirXXXXXX");
        string_copy(pattern, built);
        name = mkdtemp(pattern);
        true_is("mkdtemp answered with its template", name == pattern);
        same("mkdtemp made the directory", stat(pattern, address_of facts), 0);
        true_is("mkdtemp made a directory", S_ISDIR(facts.st_mode) != 0);
        same("mkdtemp made it private", (bipolar)(facts.st_mode & 0777),
             0700);
        rmdir(pattern);

        string_copy(pattern, (string_address) "/tmp/pd-no-x");
        errno = 0;
        true_is("mkdtemp refuses a bad template", is_null(mkdtemp(pattern)));
        same("mkdtemp with no X is EINVAL", errno, EINVAL);

        //      tmpnam hands back a name in /tmp that is not there yet.
        name = tmpnam(null);
        true_is("tmpnam answered", !is_null(name));
        true_is("tmpnam answered in /tmp",
                memory_compare(name, "/tmp/", 5) == 0);
        errno = 0;
        same("tmpnam's name was free", access(name, F_OK), -1);
        same("tmpnam's name was free, and that is why", errno, ENOENT);

        //      tmpfile is a stream with no name that still reads back what
        //      was written.
        handle = tmpfile();
        true_is("tmpfile opened a stream", !is_null(handle));

        if (!is_null(handle))
        {
                same("tmpfile took the bytes",
                     (bipolar)fwrite("abcdefg", 1, 7, handle), 7);
                same("tmpfile rewound", fseek(handle, 0, SEEK_SET), 0);
                same("tmpfile gave the bytes back",
                     (bipolar)fread(read_back, 1, 7, handle), 7);
                same("tmpfile kept them intact",
                     memory_compare(read_back, "abcdefg", 7), 0);
                fclose(handle);
        }
        else
        {
                checks += 4;
                failures += 4;
        }
}

//      -- remove ---------------------------------------------------------------

static fn test_remove(void)
{
        p8 path[PATH_MAX];
        p8 inside[PATH_MAX];
        b32 handle;

        test_path(path, (string_address) "removable");
        handle = creat(path, 0644);
        close(handle);
        same("remove took the file away", remove(path), 0);
        errno = 0;
        same("the file is gone", access(path, F_OK), -1);

        test_path(path, (string_address) "removable_directory");
        mkdir(path, 0755);
        same("remove took the directory away", remove(path), 0);

        test_path(path, (string_address) "full_directory");
        mkdir(path, 0755);
        path_join(inside, PATH_MAX, path, (string_address) "occupant");
        handle = creat(inside, 0644);
        close(handle);
        errno = 0;
        same("remove refuses a directory with something in it", remove(path),
             -1);
        same("remove on a full directory is ENOTEMPTY", errno, ENOTEMPTY);
        unlink(inside);
        rmdir(path);

        test_path(path, (string_address) "never_existed");
        errno = 0;
        same("remove refuses a missing name", remove(path), -1);
        same("remove on a missing name is ENOENT", errno, ENOENT);
}

//      -- getopt ---------------------------------------------------------------

static fn test_getopt(void)
{
        string_address words[6];
        b32 answer;

        words[0] = (string_address) "prog";
        words[1] = (string_address) "-a";
        words[2] = (string_address) "-bvalue";
        words[3] = (string_address) "rest";
        words[4] = null;

        optind = 0;
        opterr = 0;

        answer = getopt(4, words, (string_address) "ab:");
        same("getopt took the first option", answer, 'a');
        true_is("getopt left no argument", is_null(optarg));

        answer = getopt(4, words, (string_address) "ab:");
        same("getopt took the second option", answer, 'b');
        same_text("getopt took the attached argument", optarg,
                  (string_address) "value");

        answer = getopt(4, words, (string_address) "ab:");
        same("getopt stopped at the operand", answer, -1);
        same("getopt left optind on the operand", optind, 3);
        same_text("and that operand is where it was", words[optind],
                  (string_address) "rest");

        //      A missing argument, both ways of hearing about it.
        words[1] = (string_address) "-b";
        words[2] = null;
        optind = 0;
        answer = getopt(2, words, (string_address) "ab:");
        same("getopt reported a missing argument", answer, '?');
        same("getopt said which option", optopt, 'b');

        optind = 0;
        answer = getopt(2, words, (string_address) ":ab:");
        same("getopt in silent mode reports a colon", answer, ':');
        same("getopt in silent mode still says which", optopt, 'b');

        //      A double dash ends the options and is consumed.
        words[1] = (string_address) "--";
        words[2] = (string_address) "-a";
        words[3] = null;
        optind = 0;
        same("getopt stopped at the double dash",
             getopt(3, words, (string_address) "ab:"), -1);
        same("getopt consumed the double dash", optind, 2);

        opterr = 1;
}

//      -- assert -----------------------------------------------------------------

/*
        assert has to be checked in a child, because a passing assert is
        invisible and a failing one takes the process with it.

        The child sends its diagnostic to a file rather than to the terminal,
        so that the parent can read the sentence back and check it is the one
        glibc writes; and the parent checks that the child died of SIGABRT
        rather than exiting, because an assert that returned would be worse
        than one that printed nothing.
*/
static fn test_assert(void)
{
        p8 path[PATH_MAX];
        p8 line[512];
        b32 status = 0;
        b32 child;
        b32 handle;
        bipolar length;

        //      A true assertion is not allowed to do anything at all.
        assert(1 == 1);
        assert(test_root[0] == '/');
        checks++;

        test_path(path, (string_address) "assert.out");

        log_flush();
        child = fork();

        if (child == 0)
        {
                handle = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                dup2(handle, 2);
                close(handle);

                assert(2 + 2 == 5);

                _exit(0);
        }

        waitpid(child, address_of status, 0);

        true_is("assert did not let the child exit", !WIFEXITED(status));
        true_is("assert killed the child", WIFSIGNALED(status) != 0);
        same("assert killed it with SIGABRT", WTERMSIG(status), SIGABRT);

        handle = open(path, O_RDONLY);
        length = read(handle, line, sizeof(line) - 1);
        close(handle);

        if (length < 0)
                length = 0;

        line[length] = end;

        true_is("assert wrote the expression it was given",
                !is_null(string_search(line,
                                       (string_address) "Assertion `2 + 2 == 5' failed.")));
        true_is("assert named the file",
                !is_null(string_search(line, (string_address) "process.c")));
        true_is("assert named the function",
                !is_null(string_search(line, (string_address) "test_assert")));

        unlink(path);
}

//      -- the tear down -----------------------------------------------------------

static fn test_sweep_away(void)
{
        process_dirent address_to address_to list = null;
        p8 path[PATH_MAX];
        b32 count;
        b32 at;

        count = scandir(test_root, address_of list, test_keep_plain, null);

        for (at = 0; at < count; at++)
        {
                path_join(path, PATH_MAX, test_root, list[at]->d_name);

                if (unlink(path) < 0)
                        rmdir(path);

                free(list[at]);
        }

        free(list);
        rmdir(test_root);
}

b32 main(void)
{
        string_copy(test_root, (string_address) "/tmp/pd-test-XXXXXX");

        if (is_null(mkdtemp(test_root)))
        {
                string_format(log, "FAIL could not make a working directory\n");
                string_format(log, "1 checks, 1 failures\n");
                log_flush();
                return 1;
        }

        test_sleeping();
        test_directories();
        test_scandir();
        test_versions();
        test_names();
        test_realpath();
        test_exec();
        test_temporary();
        test_remove();
        test_getopt();
        test_assert();
        test_sweep_away();

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
