#include "../compiler_memory.c"

/*
        file_slurp's contract at the syscall boundary.

        Ordinary files pin every capacity edge and the guard bytes around an
        unaligned destination. Two FIFO runs force the two EINTR paths
        separately: the first signal arrives while openat is waiting for a
        writer; the second writer is already open and holds the pipe empty
        while read waits. Neither test relies on SA_RESTART -- x86_64 supplies
        its required rt_sigreturn trampoline explicitly, while arm64 and
        riscv64 use the kernel-provided return path.
*/

#ifndef LINUX
#error "file_slurp is a Linux userspace primitive"
#endif

static positive checks;
static positive failures;
static volatile positive signals_caught;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        string_format(log, "  FAIL " name "\n");        \
                }                                                       \
        } while (0)

#define SLURP_TEST_SIGNAL 10
#define SLURP_TEST_RESTORER 0x04000000
#define SLURP_TEST_FIFO_MODE (0010000 | 0600)

#define SLURP_TEST_TEXT_INNER(value) #value
#define SLURP_TEST_TEXT(value) SLURP_TEST_TEXT_INNER(value)

#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(slurp_test_signal_return)
    "mov $" SLURP_TEST_TEXT(syscall(rt_sigreturn)) ", %eax\n   syscall\n"
    ASM_END(slurp_test_signal_return)
);

fn slurp_test_signal_return(void);
#define SLURP_TEST_SIGNAL_FLAGS SLURP_TEST_RESTORER
#define SLURP_TEST_SIGNAL_RETURN ((positive)slurp_test_signal_return)
#else
#define SLURP_TEST_SIGNAL_FLAGS 0
#define SLURP_TEST_SIGNAL_RETURN 0
#endif

static fn slurp_test_signal_caught(b32 number)
{
        (void)number;
        signals_caught++;
}

static fn slurp_test_path(p8 address_to into, string_address suffix)
{
        static const string_address prefix =
              (string_address) "/tmp/moonwater-slurp-";
        positive at = string_length(prefix);
        positive suffix_length = string_length(suffix);

        memory_copy(into, prefix, at);
        at += positive_into_string(into + at,
                                   (positive)system_call(syscall(getpid)));
        memory_copy(into + at, suffix, suffix_length + 1);
}

static fn slurp_test_unlink(string_address path)
{
        system_call_3(syscall(unlinkat), (positive)(bipolar)AT_FDCWD,
                      (positive)path, 0);
}

static bool slurp_test_file(string_address path, address_any data, positive length)
{
        slurp_test_unlink(path);

        bipolar handle = system_call_4(syscall(openat),
                                       (positive)(bipolar)AT_FDCWD,
                                       (positive)path, FILE_WRITE, 0600);

        if (handle < 0)
                return false;

        positive written = system_write_all((positive)handle, data, length);
        system_call_1(syscall(close), (positive)handle);
        return written == length;
}

static bool slurp_test_guards(p8 address_to room, positive size,
                              positive at, positive capacity)
{
        for (positive i = 0; i < at; i++)
                if (room[i] != 0xa5)
                        return false;

        for (positive i = at + capacity; i < size; i++)
                if (room[i] != 0xa5)
                        return false;

        return true;
}

static bool slurp_test_all(p8 address_to room, positive size, p8 value)
{
        for (positive i = 0; i < size; i++)
                if (room[i] != value)
                        return false;

        return true;
}

static fn slurp_test_nap(void)
{
        positive duration[2] = {0, 50000000};
        system_call_2(syscall(nanosleep), (positive)duration, 0);
}

static fn slurp_test_fifo_child(string_address path, positive parent,
                                bool signal_before_open,
                                address_any data, positive length)
{
        bipolar handle;

        if (signal_before_open)
        {
                slurp_test_nap();
                system_call_2(syscall(kill), parent, SLURP_TEST_SIGNAL);
                slurp_test_nap();
        }

        handle = system_call_4(syscall(openat), (positive)(bipolar)AT_FDCWD,
                               (positive)path, 1, 0);

        if (handle < 0)
                exit(91);

        if (!signal_before_open)
        {
                slurp_test_nap();
                system_call_2(syscall(kill), parent, SLURP_TEST_SIGNAL);
                slurp_test_nap();
        }

        positive written = system_write_all((positive)handle, data, length);
        system_call_1(syscall(close), (positive)handle);
        exit(written == length ? 0 : 92);
}

static fn slurp_test_fifo(string_address path, bool signal_before_open)
{
        static p8 payload[] = "EINTR survived";
        p8 room[64];
        positive state = 0;
        positive parent = (positive)system_call(syscall(getpid));

        slurp_test_unlink(path);
        bipolar made = system_call_4(syscall(mknodat),
                                     (positive)(bipolar)AT_FDCWD,
                                     (positive)path, SLURP_TEST_FIFO_MODE, 0);
        check("FIFO made", made == 0);

        if (made < 0)
                return;

        signals_caught = 0;
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
                slurp_test_fifo_child(path, parent, signal_before_open,
                                      payload, sizeof payload - 1);

        check("FIFO child made", child > 0);

        if (child > 0)
        {
                memory_fill(room, 0xa5, sizeof room);
                bipolar got;

                if (signal_before_open)
                        got = file_slurp(path, room + 3, sizeof room - 6);
                else
                {
                        bipolar handle = system_call_4(
                              syscall(openat), (positive)(bipolar)AT_FDCWD,
                              (positive)path, FILE_READ, 0);

                        check("retry primitive FIFO opened", handle >= 0);

                        if (handle >= 0)
                        {
                                got = system_read_retry((positive)handle, room + 3,
                                                        sizeof payload - 1);
                                system_call_1(syscall(close), (positive)handle);
                        }
                        else
                        {
                                got = handle;
                                system_call_2(syscall(kill), (positive)child, 9);
                        }
                }

                system_call_4(syscall(wait4), (positive)child,
                              (positive)address_of state, 0, 0);

                check("FIFO signal caught", signals_caught != 0);
                check("FIFO child succeeded", ((state >> 8) & 0xff) == 0);
                check("FIFO slurp length", got == sizeof payload - 1);
                check("FIFO slurp bytes",
                      !memory_compare(room + 3, payload, sizeof payload - 1));

                if (signal_before_open)
                {
                        check("FIFO slurp terminator",
                              room[3 + sizeof payload - 1] == 0);
                        check("FIFO slurp guards",
                              slurp_test_guards(room, sizeof room, 3,
                                                sizeof room - 6));
                }
                else
                        check("retry primitive guards",
                              slurp_test_guards(room, sizeof room, 3,
                                                sizeof payload - 1));
        }

        slurp_test_unlink(path);
}

static fn slurp_test_regular(string_address path, string_address empty,
                             string_address absent)
{
        static p8 contents[] = "hardware floor slurp";
        p8 room[96];
        positive length = sizeof contents - 1;
        bipolar got;

        check("fixture made", slurp_test_file(path, contents, length));
        check("empty fixture made", slurp_test_file(empty, contents, 0));
        slurp_test_unlink(absent);

        memory_fill(room, 0xa5, sizeof room);
        got = system_read_retry((positive)(bipolar)-1, room + 4, 8);
        check("retry primitive preserves error", got == -9);
        check("retry primitive error leaves destination",
              slurp_test_all(room, sizeof room, 0xa5));

        bipolar handle = system_call_4(syscall(openat),
                                       (positive)(bipolar)AT_FDCWD,
                                       (positive)path, FILE_READ, 0);
        check("retry primitive regular opened", handle >= 0);

        if (handle >= 0)
        {
                memory_fill(room, 0xa5, sizeof room);
                got = system_read_retry((positive)handle, null, 0);
                check("retry primitive preserves zero count", got == 0);
                check("retry primitive zero count leaves destination",
                      slurp_test_all(room, sizeof room, 0xa5));
                system_call_1(syscall(close), (positive)handle);
        }

        handle = system_call_4(syscall(openat),
                               (positive)(bipolar)AT_FDCWD,
                               (positive)empty, FILE_READ, 0);
        check("retry primitive empty opened", handle >= 0);

        if (handle >= 0)
        {
                memory_fill(room, 0xa5, sizeof room);
                got = system_read_retry((positive)handle, room + 3, 8);
                check("retry primitive preserves EOF", got == 0);
                check("retry primitive EOF leaves destination",
                      slurp_test_all(room, sizeof room, 0xa5));
                system_call_1(syscall(close), (positive)handle);
        }

        // Zero capacity is before both pointer use and openat. A null path
        // would have returned EFAULT if the syscall had happened.
        check("capacity zero accepts nulls", file_slurp(null, null, 0) == 0);
        memory_fill(room, 0xa5, sizeof room);
        check("capacity zero skips open", file_slurp(absent, room + 5, 0) == 0);
        check("capacity zero leaves destination",
              slurp_test_all(room, sizeof room, 0xa5));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(absent, room + 5, 1);
        check("capacity one still opens", got == -2);
        check("capacity one open error leaves destination",
              slurp_test_all(room, sizeof room, 0xa5));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(path, room + 5, 1);
        check("capacity one length", got == 0);
        check("capacity one terminator", room[5] == 0);
        check("capacity one guards",
              slurp_test_guards(room, sizeof room, 5, 1));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(path, room + 3, 6);
        check("truncated length", got == 5);
        check("truncated bytes", !memory_compare(room + 3, contents, 5));
        check("truncated terminator", room[8] == 0);
        check("truncated guards",
              slurp_test_guards(room, sizeof room, 3, 6));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(path, room + 7, length + 1);
        check("exact capacity length", got == (bipolar)length);
        check("exact capacity bytes",
              !memory_compare(room + 7, contents, length));
        check("exact capacity terminator", room[7 + length] == 0);
        check("exact capacity guards",
              slurp_test_guards(room, sizeof room, 7, length + 1));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(path, room + 1, length + 8);
        check("EOF length", got == (bipolar)length);
        check("EOF bytes", !memory_compare(room + 1, contents, length));
        check("EOF terminator", room[1 + length] == 0);
        check("EOF guards",
              slurp_test_guards(room, sizeof room, 1, length + 8));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(empty, room + 9, 8);
        check("empty length", got == 0);
        check("empty terminator", room[9] == 0);
        check("empty guards", slurp_test_guards(room, sizeof room, 9, 8));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp(absent, room + 4, 8);
        check("open error", got == -2);
        check("open error leaves destination",
              slurp_test_all(room, sizeof room, 0xa5));

        memory_fill(room, 0xa5, sizeof room);
        got = file_slurp((string_address) "/tmp", room + 5, 8);
        check("directory read error", got == -21);
        check("directory error terminator", room[5] == 0);
        check("directory error guards",
              slurp_test_guards(room, sizeof room, 5, 8));

        slurp_test_unlink(path);
        slurp_test_unlink(empty);
}

b32 main(void)
{
        p8 regular[128], empty[128], absent[128], open_fifo[128], read_fifo[128];
        positive action[4] = {(positive)slurp_test_signal_caught,
                              SLURP_TEST_SIGNAL_FLAGS,
                              SLURP_TEST_SIGNAL_RETURN, 0};

        slurp_test_path(regular, (string_address) "-regular");
        slurp_test_path(empty, (string_address) "-empty");
        slurp_test_path(absent, (string_address) "-absent");
        slurp_test_path(open_fifo, (string_address) "-open-fifo");
        slurp_test_path(read_fifo, (string_address) "-read-fifo");

        check("signal action installed",
              system_call_4(syscall(rt_sigaction), SLURP_TEST_SIGNAL,
                            (positive)action, 0, 8) == 0);

        slurp_test_regular(regular, empty, absent);
        slurp_test_fifo(open_fifo, true);
        slurp_test_fifo(read_fifo, false);

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();
        return failures ? 1 : 0;
}

#undef SLURP_TEST_TEXT
#undef SLURP_TEST_TEXT_INNER
