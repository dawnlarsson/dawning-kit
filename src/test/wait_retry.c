#include "../compiler_memory.c"

/*
        The wait4 retry leaf at the syscall boundary.

        A child reports the pid it is about to wait for, then has no blocking
        operation except wait4. Its parent polls /proc/<pid>/stat until that
        waiter is observably asleep, sends a non-restarting signal, and waits
        for the handler's pipe acknowledgement before releasing the worker.
        Scheduling luck before wait4 therefore cannot satisfy the assertions.

        Supplying an unaligned rusage block proves the fourth argument survived
        that retry (and, on x86_64, reached r10). A second child pins WNOHANG
        before it is collected normally, and waiting twice for the worker pins
        every non-EINTR error unchanged.
*/

#ifndef LINUX
#error "system_wait4_retry is a Linux userspace primitive"
#endif

#include "counted.inc"

static volatile positive signals_caught;
static bipolar wait_retry_signal_report = -1;

#define WAIT_RETRY_SIGNAL 10
#define WAIT_RETRY_RESTORER 0x04000000
#define WAIT_RETRY_USAGE 144
#define WAIT_RETRY_NO_HANG 1
#define WAIT_RETRY_REPORT_ALL 0x3f

#define WAIT_RETRY_TEXT_INNER(value) #value
#define WAIT_RETRY_TEXT(value) WAIT_RETRY_TEXT_INNER(value)

#if X64
__asm__(
    ASM_SECTION
    ASM_FUNC(wait_retry_signal_return)
    "mov $" WAIT_RETRY_TEXT(syscall(rt_sigreturn)) ", %eax\n   syscall\n"
    ASM_END(wait_retry_signal_return)
);

fn wait_retry_signal_return(void);
#define WAIT_RETRY_SIGNAL_FLAGS WAIT_RETRY_RESTORER
#define WAIT_RETRY_SIGNAL_RETURN ((positive)wait_retry_signal_return)
#else
#define WAIT_RETRY_SIGNAL_FLAGS 0
#define WAIT_RETRY_SIGNAL_RETURN 0
#endif

static fn wait_retry_signal_caught(b32 number)
{
        p8 report = 0x5a;

        (void)number;
        signals_caught++;

        if (wait_retry_signal_report >= 0)
                system_call_3(syscall(write),
                              (positive)wait_retry_signal_report,
                              (positive)address_of report, 1);
}

static fn wait_retry_nap(void)
{
        positive duration[2] = {0, 50000000};
        system_call_2(syscall(nanosleep), (positive)duration, 0);
}

static fn wait_retry_poll_nap(void)
{
        positive duration[2] = {0, 1000000};
        system_call_2(syscall(nanosleep), (positive)duration, 0);
}

static fn wait_retry_child(bipolar read_end, bipolar write_end, b32 status)
{
        p8 release = 0;

        system_call_1(syscall(close), (positive)write_end);
        system_read_retry((positive)read_end, address_of release, 1);
        system_call_1(syscall(close), (positive)read_end);

        exit(status);
}

static fn wait_retry_proc_path(p8 address_to into, bipolar process)
{
        static p8 prefix[] = "/proc/";
        static p8 suffix[] = "/stat";
        positive at = sizeof prefix - 1;

        memory_copy(into, prefix, at);
        at += positive_into_string(into + at, (positive)process);
        memory_copy(into + at, suffix, sizeof suffix);
}

static bool wait_retry_observe_wait(bipolar process)
{
        p8 path[64], state[512];

        wait_retry_proc_path(path, process);

        for (positive attempt = 0; attempt < 2000; attempt++)
        {
                bipolar length = file_slurp(path, state, sizeof state);

                if (length > 3)
                {
                        bipolar close = -1;

                        for (bipolar i = 0; i < length; i++)
                                if (state[i] == ')')
                                        close = i;

                        if (close >= 0 && close + 2 < length && state[close + 2] == 'S')
                                return true;
                }

                wait_retry_poll_nap();
        }

        return false;
}

static bool wait_retry_usage_changed(p8 address_to room)
{
        for (positive i = 3; i < 3 + WAIT_RETRY_USAGE; i++)
                if (room[i] != 0xa5)
                        return true;

        return false;
}

static bool wait_retry_usage_guards(p8 address_to room, positive size)
{
        for (positive i = 0; i < 3; i++)
                if (room[i] != 0xa5)
                        return false;

        for (positive i = 3 + WAIT_RETRY_USAGE; i < size; i++)
                if (room[i] != 0xa5)
                        return false;

        return true;
}

static fn wait_retry_worker(bipolar read_end, bipolar write_end)
{
        system_call_1(syscall(close), (positive)read_end);
        system_call_1(syscall(close), (positive)write_end);

        for (;;)
                wait_retry_nap();
}

static fn wait_retry_waiter(bipolar read_end, bipolar write_end)
{
        p8 usage[WAIT_RETRY_USAGE + 16];
        positive status = 0;
        positive report = 0;

        system_call_1(syscall(close), (positive)read_end);
        memory_fill(usage, 0xa5, sizeof usage);
        signals_caught = 0;
        wait_retry_signal_report = write_end;

        bipolar worker = system_call_2(syscall(clone), SIGCHLD, 0);

        if (worker == 0)
                wait_retry_worker(read_end, write_end);

        system_write_all((positive)write_end, address_of worker, sizeof worker);

        if (worker > 0)
        {
                bipolar got = system_wait4_retry(worker, address_of status, 0,
                                                 usage + 3);

                if (got == worker)
                        report |= 1;

                if (signals_caught)
                        report |= 2;

                if ((status & 0x7f) == 9)
                        report |= 4;

                if (wait_retry_usage_changed(usage))
                        report |= 8;

                if (wait_retry_usage_guards(usage, sizeof usage))
                        report |= 16;

                if (system_wait4_retry(worker, address_of status, 0, null) == -10)
                        report |= 32;
        }

        system_write_all((positive)write_end, address_of report, sizeof report);
        system_call_1(syscall(close), (positive)write_end);
        exit(report == WAIT_RETRY_REPORT_ALL ? 0 : 90);
}

static fn wait_retry_interrupted()
{
        b32 channel[2] = {-1, -1};
        bipolar worker = -1;
        positive report = 0;
        positive status = 0;

        check("interrupt channel made",
              system_call_2(syscall(pipe2), (positive)channel, 0) == 0);

        if (channel[0] < 0)
                return;

        bipolar waiter = system_call_2(syscall(clone), SIGCHLD, 0);

        if (waiter == 0)
                wait_retry_waiter(channel[0], channel[1]);

        system_call_1(syscall(close), (positive)channel[1]);
        check("interrupt waiter made", waiter > 0);

        if (waiter > 0)
        {
                check("worker pid reported",
                      system_read_retry((positive)channel[0], address_of worker,
                                        sizeof worker) == sizeof worker && worker > 0);

                bool observed = worker > 0 && wait_retry_observe_wait(waiter);
                check("wait4 observed before signal", observed);

                if (observed)
                {
                        p8 acknowledged = 0;

                        check("waiter signal sent",
                              system_call_2(syscall(kill), (positive)waiter,
                                            WAIT_RETRY_SIGNAL) == 0);
                        check("interrupt handler acknowledged",
                              system_read_retry((positive)channel[0],
                                                address_of acknowledged, 1) == 1 &&
                                    acknowledged == 0x5a);
                        check("worker release sent",
                              system_call_2(syscall(kill), (positive)worker, 9) == 0);
                }
                else
                {
                        system_call_2(syscall(kill), (positive)waiter, 9);

                        if (worker > 0)
                                system_call_2(syscall(kill), (positive)worker, 9);
                }

                check("waiter report returned",
                      system_read_retry((positive)channel[0], address_of report,
                                        sizeof report) == sizeof report);
                check("interrupted wait returns child", report & 1);
                check("interrupt handler ran", report & 2);
                check("interrupted wait status", report & 4);
                check("retry preserves rusage", report & 8);
                check("rusage guards", report & 16);
                check("non-EINTR error preserved", report & 32);
                check("waiter succeeded",
                      system_wait4_retry(waiter, address_of status, 0, null) == waiter &&
                            wait_status_code(status) == 0);
        }

        system_call_1(syscall(close), (positive)channel[0]);
}

static fn wait_retry_nohang()
{
        b32 channel[2] = {-1, -1};
        p8 release = 1;
        positive status = 0;

        check("nohang channel made",
              system_call_2(syscall(pipe2), (positive)channel, 0) == 0);

        if (channel[0] < 0)
                return;

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
                wait_retry_child(channel[0], channel[1], 19);

        system_call_1(syscall(close), (positive)channel[0]);

        check("nohang child made", child > 0);

        if (child > 0)
        {
                check("WNOHANG forwarded",
                      system_wait4_retry(child, address_of status,
                                         WAIT_RETRY_NO_HANG, null) == 0);
                check("nohang child released",
                      system_write_all((positive)channel[1],
                                       address_of release, 1) == 1);
                check("nohang child collected",
                      system_wait4_retry(child, address_of status, 0, null) == child);
                check("nohang child status", wait_status_code(status) == 19);
        }

        system_call_1(syscall(close), (positive)channel[1]);
}

b32 main(void)
{
        positive action[4] = {(positive)wait_retry_signal_caught,
                              WAIT_RETRY_SIGNAL_FLAGS,
                              WAIT_RETRY_SIGNAL_RETURN, 0};

        check("signal action installed",
              system_call_4(syscall(rt_sigaction), WAIT_RETRY_SIGNAL,
                            (positive)action, 0, 8) == 0);

        wait_retry_interrupted();
        wait_retry_nohang();

        return test_report(null);
}

#undef WAIT_RETRY_TEXT
#undef WAIT_RETRY_TEXT_INNER
