/*
        The same transcript, out of the real C library.

        src/test/error.c prints a stream of lines beginning TRANSCRIPT. This
        program prints the same stream using glibc's strerror, strerror_r,
        perror and syscall wrappers, so the check is

            diff <(reference) <(grep TRANSCRIPT freestanding)

        and any disagreement about a message, a return value or an errno is a
        line in that diff. It is built and linked normally -- it is the one
        file in the tree that is not freestanding, and it exists only to be
        disagreed with.

        _GNU_SOURCE is deliberately absent. glibc chooses between the POSIX
        strerror_r, which returns int, and the GNU one, which returns char *,
        on that macro, and the POSIX one is the contract src/standard/error.c
        implements.
*/

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

//      dup3 and pipe2 are behind _GNU_SOURCE in the headers, and _GNU_SOURCE
//      would swap strerror_r for the char * returning one this does not test.
//      They are in the library either way, so they are declared here.
extern int dup3(int from, int to, int flags);
extern int pipe2(int pair[2], int flags);

#define ERROR_TEST_DIRECTORY "/tmp/dawning-error-test"
#define ERROR_TEST_FILE "/tmp/dawning-error-test/one"
#define ERROR_TEST_LINK "/tmp/dawning-error-test/link"
#define ERROR_TEST_ABSENT "/tmp/dawning-error-test/absent"

static void shown(char *into, const char *buffer, size_t count)
{
        size_t at;

        for (at = 0; at < count; at++)
                into[at] = buffer[at] == 0 ? '|' : buffer[at];

        into[count] = 0;
}

static void strerror_r_line(int number, size_t size)
{
        char buffer[128];
        char seen[136];
        int answer;

        memset(buffer, '@', sizeof buffer);
        answer = strerror_r(number, buffer, size);
        shown(seen, buffer, size + 2 < 128 ? size + 2 : 128);

        printf("TRANSCRIPT strerror_r %d %zu rc=%d [%s]\n", number, size,
               answer, seen);
}

static void messages(void)
{
        int number;

        for (number = 0; number <= 140; number++)
                printf("TRANSCRIPT strerror %d [%s]\n", number,
                       strerror(number));

        printf("TRANSCRIPT strerror %d [%s]\n", -1, strerror(-1));
        printf("TRANSCRIPT strerror %d [%s]\n", -7, strerror(-7));
        printf("TRANSCRIPT strerror %d [%s]\n", 1000, strerror(1000));

        strerror_r_line(2, 64);
        strerror_r_line(2, 26);
        strerror_r_line(2, 25);
        strerror_r_line(2, 24);
        strerror_r_line(2, 10);
        strerror_r_line(2, 1);
        strerror_r_line(2, 0);
        strerror_r_line(0, 8);
        strerror_r_line(0, 7);
        strerror_r_line(84, 49);
        strerror_r_line(84, 48);
        strerror_r_line(41, 64);
        strerror_r_line(41, 10);
        strerror_r_line(41, 0);
        strerror_r_line(58, 64);
        strerror_r_line(133, 64);
        strerror_r_line(134, 64);
        strerror_r_line(-7, 64);
}

static void perror_lines(void)
{
        fflush(stdout);
        errno = ENOENT;
        perror("TRANSCRIPT perror named");

        fflush(stdout);
        errno = ENOENT;
        perror("");

        fflush(stdout);
        errno = ENOENT;
        perror(NULL);

        fflush(stdout);
        errno = 41;
        perror("TRANSCRIPT perror unknown");

        fflush(stdout);
        errno = 0;
        perror("TRANSCRIPT perror success");
}

static void said(const char *what, long result)
{
        printf("TRANSCRIPT call %s -> %ld errno %d\n", what, result, errno);
}

static void wrappers(void)
{
        char room[64];
        char small[4];
        int pair[2];
        int handle;
        struct stat one;
        long got;

        unlink(ERROR_TEST_LINK);
        unlink(ERROR_TEST_FILE);
        rmdir(ERROR_TEST_DIRECTORY);

        errno = 0;
        said("mkdir", mkdir(ERROR_TEST_DIRECTORY, 0755));

        errno = 0;
        said("mkdir-again", mkdir(ERROR_TEST_DIRECTORY, 0755));

        errno = 0;
        handle = open(ERROR_TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        said("open-create", handle >= 0);

        errno = 0;
        said("write", write(handle, "abcdefgh", 8));

        errno = 0;
        said("lseek-end", lseek(handle, 0, SEEK_END));

        errno = 0;
        said("read-on-write-only", read(handle, room, 4));

        errno = 0;
        said("close", close(handle));

        errno = 0;
        said("close-twice", close(handle));

        errno = 0;
        said("open-missing", open(ERROR_TEST_ABSENT, O_RDONLY));

        errno = 0;
        said("open-exclusive-existing",
             open(ERROR_TEST_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644));

        errno = 0;
        said("open-directory-for-write", open(ERROR_TEST_DIRECTORY, O_WRONLY));

        errno = 0;
        said("stat", stat(ERROR_TEST_FILE, &one));

        printf("TRANSCRIPT stat size %ld regular %d directory %d\n",
               (long)one.st_size, S_ISREG(one.st_mode) ? 1 : 0,
               S_ISDIR(one.st_mode) ? 1 : 0);

        printf("TRANSCRIPT stat permission %lu links %ld\n",
               (unsigned long)(one.st_mode & 07777), (long)one.st_nlink);

        errno = 0;
        said("stat-missing", stat(ERROR_TEST_ABSENT, &one));

        errno = 0;
        said("stat-directory", stat(ERROR_TEST_DIRECTORY, &one));

        printf("TRANSCRIPT stat directory-bit %d\n",
               S_ISDIR(one.st_mode) ? 1 : 0);

        errno = 0;
        said("symlink", symlink("one", ERROR_TEST_LINK));

        errno = 0;
        memset(room, '@', sizeof room);
        got = readlink(ERROR_TEST_LINK, room, sizeof room);
        said("readlink", got);

        errno = 0;
        said("readlink-not-a-link", readlink(ERROR_TEST_FILE, room, sizeof room));

        errno = 0;
        said("lstat-is-link", lstat(ERROR_TEST_LINK, &one));

        printf("TRANSCRIPT lstat link-bit %d\n", S_ISLNK(one.st_mode) ? 1 : 0);

        errno = 0;
        said("access-existing", access(ERROR_TEST_FILE, F_OK));

        errno = 0;
        said("access-missing", access(ERROR_TEST_ABSENT, F_OK));

        errno = 0;
        said("rmdir-not-empty", rmdir(ERROR_TEST_DIRECTORY));

        errno = 0;
        said("unlink-missing", unlink(ERROR_TEST_ABSENT));

        errno = 0;
        said("rename", rename(ERROR_TEST_FILE, ERROR_TEST_ABSENT));

        errno = 0;
        said("rename-back", rename(ERROR_TEST_ABSENT, ERROR_TEST_FILE));

        errno = 0;
        said("rename-missing", rename(ERROR_TEST_ABSENT, ERROR_TEST_LINK));

        errno = 0;
        said("pipe", pipe(pair));

        errno = 0;
        said("lseek-on-pipe", lseek(pair[0], 0, SEEK_SET));

        errno = 0;
        said("isatty-on-pipe", isatty(pair[0]));

        errno = 0;
        said("isatty-bad-handle", isatty(999));

        errno = 0;
        said("dup2-onto-self", dup2(pair[0], pair[0]));

        errno = 0;
        said("dup2-closed-onto-self", dup2(900, 900));

        errno = 0;
        said("close-pipe-read", close(pair[0]));

        errno = 0;
        said("close-pipe-write", close(pair[1]));

        errno = 0;
        said("write-bad-handle", write(999, "x", 1));

        errno = 0;
        said("getcwd-too-small", getcwd(small, sizeof small) == NULL);

        errno = 0;
        said("getpid-positive", getpid() > 0);

        errno = 0;
        said("chmod", chmod(ERROR_TEST_FILE, 0600));

        errno = 0;
        said("stat-after-chmod", stat(ERROR_TEST_FILE, &one));

        printf("TRANSCRIPT stat permission %lu\n",
               (unsigned long)(one.st_mode & 07777));

        errno = 0;
        said("chmod-missing", chmod(ERROR_TEST_ABSENT, 0600));

        errno = 0;
        said("unlink-link", unlink(ERROR_TEST_LINK));

        errno = 0;
        said("unlink", unlink(ERROR_TEST_FILE));

        errno = 0;
        said("rmdir", rmdir(ERROR_TEST_DIRECTORY));
}


#define ERROR_MORE_DIRECTORY "/tmp/dawning-error-more"
#define ERROR_MORE_FILE "/tmp/dawning-error-more/two"
#define ERROR_MORE_ABSENT "/tmp/dawning-error-more/absent/deeper"

//      The mirror of error_test_more. renameat rather than renameat2: the
//      freestanding side has to call renameat2 because riscv64 has no
//      renameat at all, and renameat2 with no flags is defined to be the
//      same call.
static void more(void)
{
        char room[64];
        int pair[2];
        int handle;
        int copy;
        int status;
        int child;
        mode_t mask;
        struct stat one;
        struct stat tagged;
        void *region;

        rmdir(ERROR_MORE_DIRECTORY);

        errno = 0;
        said("more-mkdir", mkdir(ERROR_MORE_DIRECTORY, 0755));

        errno = 0;
        handle = creat(ERROR_MORE_FILE, 0644);
        said("creat", handle >= 0);

        errno = 0;
        said("creat-missing-directory", creat(ERROR_MORE_ABSENT, 0644));

        errno = 0;
        said("pwrite", pwrite(handle, "abcdefgh", 8, 0));

        errno = 0;
        memset(room, '@', sizeof room);
        said("pread", pread(handle, room, 4, 2));

        room[4] = 0;
        printf("TRANSCRIPT pread saw [%s]\n", room);

        errno = 0;
        said("pread-bad-handle", pread(-1, room, 4, 0));

        errno = 0;
        said("pwrite-bad-handle", pwrite(-1, "x", 1, 0));

        errno = 0;
        said("fstat", fstat(handle, &one));

        printf("TRANSCRIPT fstat size %ld regular %d\n", (long)one.st_size,
               S_ISREG(one.st_mode) ? 1 : 0);

        errno = 0;
        said("fstat-through-the-tag", fstat(handle, &tagged));

        printf("TRANSCRIPT tagged size %ld\n", (long)tagged.st_size);

        errno = 0;
        said("fstat-bad-handle", fstat(-1, &one));

        errno = 0;
        said("fchmod", fchmod(handle, 0600));

        errno = 0;
        said("fchmod-bad-handle", fchmod(-1, 0600));

        errno = 0;
        said("fchown-no-change", fchown(handle, (uid_t)-1, (gid_t)-1));

        errno = 0;
        said("fchown-bad-handle", fchown(-1, (uid_t)-1, (gid_t)-1));

        errno = 0;
        said("ftruncate", ftruncate(handle, 4));

        errno = 0;
        said("ftruncate-bad-handle", ftruncate(-1, 4));

        errno = 0;
        said("fsync", fsync(handle));

        errno = 0;
        said("fsync-bad-handle", fsync(-1));

        errno = 0;
        said("fdatasync", fdatasync(handle));

        errno = 0;
        said("fdatasync-bad-handle", fdatasync(-1));

        errno = 0;
        said("isatty-on-file", isatty(handle));

        errno = 0;
        said("fcntl-getfd", fcntl(handle, F_GETFD, 0L));

        errno = 0;
        said("fcntl-bad-handle", fcntl(-1, F_GETFD, 0L));

        errno = 0;
        said("ioctl-bad-handle", ioctl(-1, 0x5401, room));

        errno = 0;
        said("close-more", close(handle));

        errno = 0;
        said("truncate", truncate(ERROR_MORE_FILE, 2));

        errno = 0;
        said("truncate-missing", truncate(ERROR_MORE_ABSENT, 0));

        errno = 0;
        handle = openat(AT_FDCWD, ERROR_MORE_FILE, O_RDONLY);
        said("openat", handle >= 0);

        errno = 0;
        said("close-openat", close(handle));

        errno = 0;
        said("openat-bad-directory", openat(-1, "relative-name", O_RDONLY));

        errno = 0;
        said("fstatat", fstatat(AT_FDCWD, ERROR_MORE_FILE, &one, 0));

        errno = 0;
        said("fstatat-bad-directory", fstatat(-1, "relative-name", &one, 0));

        errno = 0;
        said("faccessat", faccessat(AT_FDCWD, ERROR_MORE_FILE, F_OK, 0));

        errno = 0;
        said("faccessat-bad-directory",
             faccessat(-1, "relative-name", F_OK, 0));

        errno = 0;
        said("mkdirat-bad-directory", mkdirat(-1, "relative-name", 0755));

        errno = 0;
        said("unlinkat-bad-directory", unlinkat(-1, "relative-name", 0));

        errno = 0;
        said("readlinkat-bad-directory",
             readlinkat(-1, "relative-name", room, sizeof room));

        errno = 0;
        said("renameat-missing", renameat(AT_FDCWD, ERROR_MORE_ABSENT,
                                          AT_FDCWD, ERROR_MORE_ABSENT));

        errno = 0;
        said("link-missing", link(ERROR_MORE_ABSENT, ERROR_MORE_ABSENT));

        errno = 0;
        said("link", link(ERROR_MORE_FILE, "/tmp/dawning-error-more/three"));

        errno = 0;
        said("unlink-link-two", unlink("/tmp/dawning-error-more/three"));

        errno = 0;
        said("chown-no-change", chown(ERROR_MORE_FILE, (uid_t)-1, (gid_t)-1));

        errno = 0;
        said("chown-missing", chown(ERROR_MORE_ABSENT, (uid_t)-1, (gid_t)-1));

        errno = 0;
        said("lchown-no-change",
             lchown(ERROR_MORE_FILE, (uid_t)-1, (gid_t)-1));

        errno = 0;
        said("lchown-missing", lchown(ERROR_MORE_ABSENT, (uid_t)-1, (gid_t)-1));

        errno = 0;
        copy = dup(1);
        said("dup", copy >= 3);

        errno = 0;
        said("close-dup", close(copy));

        errno = 0;
        said("dup-bad-handle", dup(-1));

        errno = 0;
        said("dup3", dup3(1, 7, 0));

        errno = 0;
        said("close-dup3", close(7));

        errno = 0;
        said("dup3-bad-handle", dup3(-1, 7, 0));

        errno = 0;
        said("pipe2", pipe2(pair, 0));

        errno = 0;
        said("close-pipe2-read", close(pair[0]));

        errno = 0;
        said("close-pipe2-write", close(pair[1]));

        errno = 0;
        said("chdir", chdir(ERROR_MORE_DIRECTORY));

        errno = 0;
        said("chdir-back", chdir("/tmp"));

        errno = 0;
        said("chdir-missing", chdir(ERROR_MORE_ABSENT));

        errno = 0;
        said("fchdir-bad-handle", fchdir(-1));

        umask(0022);
        mask = umask(0077);
        printf("TRANSCRIPT umask previous %lu\n", (unsigned long)mask);
        umask(mask);

        errno = 0;
        region = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        said("mmap", region != MAP_FAILED);

        memset(region, 0x5a, 4096);

        errno = 0;
        said("mprotect", mprotect(region, 4096, PROT_READ));

        errno = 0;
        said("munmap", munmap(region, 4096));

        errno = 0;
        said("mmap-zero-length",
             mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                     == MAP_FAILED);

        errno = 0;
        said("munmap-zero-length", munmap(NULL, 0));

        printf("TRANSCRIPT identity ppid-positive %d uid-is-euid %d"
               " gid-is-egid %d\n",
               getppid() > 0, getuid() == geteuid(), getgid() == getegid());

        errno = 0;
        said("kill-probe-self", kill(getpid(), 0));

        errno = 0;
        said("kill-bad-signal", kill(getpid(), 9999));

        errno = 0;
        said("execve-missing",
             execve("/nonexistent-dawning-execve", NULL, NULL));

        errno = 0;
        said("sync", (sync(), 0));

        fflush(stdout);

        errno = 0;
        child = fork();

        if (child == 0)
                _exit(7);

        status = 0;
        errno = 0;
        said("fork-then-waitpid", waitpid(child, &status, 0) == child);

        printf("TRANSCRIPT child status %d\n", status);

        fflush(stdout);

        errno = 0;
        child = fork();

        if (child == 0)
                _Exit(3);

        status = 0;
        errno = 0;
        said("fork-then-wait", wait(&status) == child);

        printf("TRANSCRIPT second child status %d\n", status);

        errno = 0;
        said("waitpid-no-children", waitpid(-1, &status, 0));

        errno = 0;
        said("wait4-no-children", wait4(-1, &status, 0, NULL));

        errno = 0;
        said("more-unlink", unlink(ERROR_MORE_FILE));

        errno = 0;
        said("more-rmdir", rmdir(ERROR_MORE_DIRECTORY));
}

int main(void)
{
        dup2(1, 2);

        messages();
        perror_lines();
        wrappers();
        more();

        fflush(stdout);
        return 0;
}
