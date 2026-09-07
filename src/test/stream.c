/*
        Experimental C standard library

        the FILE trace, over this tree's own streams

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        The trace goes through log, which is the library's own buffered writer
        on descriptor one and has nothing to do with the FILE streams being
        measured. A trace printed through the thing it is measuring cannot
        report on it: a stream that flushed at the wrong moment would move the
        evidence rather than show it.
*/
#include "../compiler_memory.c"

#define trace_number(label, value)                                    \
        string_format(log, "%s %b\n", (string_address)(label),        \
                      (b32)(long)(value))

#define body_limit_memory() \
        (system_call_4(syscall(prlimit64), 0, 9, \
                        (positive)(p64[]){1, 1}, 0) == 0)
#include "stream_body.c"

static bool dynamic_buffer_fits_one_shelf(void)
{
        stream address_to handle =
                stream_open((string_address)"/dev/null", (string_address)"w");

        if (is_null(handle))
                return false;

        stream_ready(handle);
        bool fits = handle->buffer_size == STREAM_DYNAMIC_BUFFER &&
                    memory_usable_size(handle->buffer) ==
                            STREAM_DYNAMIC_BUFFER;

        stream_close(handle);
        return fits;
}

static bool empty_mode_stays_bounded(void)
{
        // 64 KiB is a multiple of every supported Linux base page size.
        positive page = 64 * 1024;
        p8 address_to bytes = memory(page * 2);
        if (!bytes || system_failed(bytes))
                return false;
        if (system_call_3(syscall(mprotect), (positive)(bytes + page), page, 0))
        {
                memory_free(bytes, page * 2);
                return false;
        }

        p8 address_to mode = bytes + page - 1;
        *mode = end;
        errno = 0;
        stream address_to opened = fopen("/dev/null", mode);
        bool bounded = !opened && errno == EINVAL;
        if (opened)
                fclose(opened);
        memory_free(bytes, page * 2);
        return bounded;
}

static fn interrupt_write(b32 number) { (void)number; }

static bool bounded_line_reads(void)
{
        p8 bytes[257], copied[260];
        static const positive limits[] = {0, 1, 2, 3, 7, 16, 63, 127, 256, 257, 259};
        for (positive delimiter = 0; delimiter < 256; delimiter++)
                for (positive stop = 0; stop <= sizeof(bytes); stop++)
                        for (positive row = 0; row < array_count(limits); row++)
                        {
                                positive limit = limits[row];
                                memory_fill(bytes, (p8)(delimiter + 1), sizeof(bytes));
                                if (stop < sizeof(bytes)) bytes[stop] = (p8)delimiter;
                                memory_fill(copied, 0xa5, sizeof(copied));
                                stream handle = {.descriptor = -1, .buffer = bytes,
                                    .read_tail = sizeof(bytes)};
                                bool found = true, ended = true;
                                positive want = stop < sizeof(bytes) ? stop + 1 : stop;
                                if (want > limit) want = limit;
                                positive got = stream_take_line(&handle, copied, limit,
                                    (b32)delimiter, &found, &ended);
                                if (got != want || handle.read_head != want || ended ||
                                    found != (stop < sizeof(bytes) && stop < limit) ||
                                    memory_compare(copied, bytes, want) || copied[want] != 0xa5)
                                        return false;
                        }
        return true;
}

static bool checked_write_errors(void)
{
        system_write_result result = system_write_all_checked(-1, address_bad, 0);
        if (result.bytes || result.error) return false;
        result = system_write_all_checked(-1, "x", 1);
        if (result.bytes || result.error != -EBADF) return false;
        b32 sink = open("/dev/null", O_WRONLY, 0);
        if (sink < 0) return false;
        errno = EDOM;
        result = system_write_all_checked(sink, "x", 1);
        bool correct = result.bytes == 1 && !result.error && errno == EDOM;
        FILE *closed = fdopen(sink, "w");
        if (!closed || setvbuf(closed, 0, _IONBF, 0)) return false;
        close(sink);
        errno = 0;
        correct &= fputc('x', closed) == EOF && errno == EBADF && ferror(closed);
        fclose(closed);
        sink = open("/dev/full", O_WRONLY, 0);
        if (sink < 0) return false;
        result = system_write_all_checked(sink, "x", 1);
        correct &= !result.bytes && result.error == -ENOSPC;
        close(sink);

        b32 ends[2];
        if (pipe2(ends, O_NONBLOCK)) return false;
        result = system_write_all_checked(ends[1], address_bad, 1);
        correct &= !result.bytes && result.error == -EFAULT;
        bipolar capacity = fcntl(ends[1], 1032, 0UL);
        if (capacity <= 0) return false;
        positive length = (positive)capacity + 4096;
        p8 *bytes = malloc(length);
        if (!bytes) return false;
        memory_fill(bytes, 0x5a, length);
        result = system_write_all_checked(ends[1], bytes, length);
        correct &= result.bytes == (positive)capacity && result.error == -EAGAIN;
        correct &= read(ends[0], bytes, length) == capacity;
        FILE *f = fdopen(ends[1], "w");
        if (!f || setvbuf(f, 0, _IONBF, 0)) return false;
        errno = 0;
        correct &= fwrite(bytes, 1, length, f) == (positive)capacity &&
                   errno == EAGAIN && ferror(f);
        clearerr(f);

        /* Leave the pipe full, block, and interrupt without SA_RESTART. The
           periodic timer also prevents a lost first signal hanging the test. */
        signal_action wanted = { .handler = interrupt_write }, previous;
        if (sigaction(SIGALRM, &wanted, &previous) ||
            fcntl(ends[1], 4, 0UL)) return false;
        p64 timer[4] = {0, 20000, 0, 20000};
        if (system_call_3(syscall(setitimer), 0, (positive)timer, 0)) return false;
        correct &= system_write_all(ends[1], "x", 1) == 0;
        result = system_write_all_checked(ends[1], "x", 1);
        correct &= !result.bytes && result.error == -EINTR;
        errno = 0;
        correct &= fputc('x', f) == EOF && errno == EINTR && ferror(f);
        memory_fill(timer, 0, sizeof(timer));
        system_call_3(syscall(setitimer), 0, (positive)timer, 0);
        sigaction(SIGALRM, &previous, 0);
        clearerr(f);
        correct &= read(ends[0], bytes, length) == capacity;
        for (positive i = 0; i < (positive)capacity; i++)
                correct &= bytes[i] == 0x5a;
        close(ends[0]);
        wanted.handler = SIG_IGN;
        if (sigaction(SIGPIPE, &wanted, &previous)) return false;
        result = system_write_all_checked(ends[1], "x", 1);
        correct &= !result.bytes && result.error == -EPIPE;
        errno = 0;
        correct &= fputc('x', f) == EOF && errno == EPIPE && ferror(f);
        sigaction(SIGPIPE, &previous, 0);
        fclose(f);
        free(bytes);
        return correct;
}

/* Native strace injects every write; no trace output can mask a failure. */
static b32 injected_write_stop(b32 expected)
{
        b32 sink = open("/dev/null", O_WRONLY, 0);
        if (sink < 0) return 1;
        system_write_result result = system_write_all_checked(sink, "x", 1);
        bool correct = !result.bytes && result.error == -expected &&
                       system_write_all(sink, "x", 1) == 0;
        errno = 0;
        correct &= stream_trap_write(sink, "x", 1) == 0 &&
                   errno == (expected ? expected : EIO);
        close(sink);
        b32 next = dup(1);
        close(next);
        errno = 0;
        FILE *f = fmemopen("x", 1, "r");
        correct &= !f && errno == (expected ? expected : EIO);
        if (f) fclose(f);
        sink = dup(1);
        correct &= sink == next;
        close(sink);
        return correct ? 0 : 1;
}

static bool bounded_stream_positions(void)
{
        b32 descriptor = (b32)system_call_2(syscall(memfd_create),
                                            (positive)"stream-positions", 1);
        if (descriptor < 0) return false;
        FILE *file = fdopen(descriptor, "w+");
        if (!file) { close(descriptor); return false; }
        bool okay = fputs("ab", file) >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
                    fgetc(file) == 'a';
        errno = 0;
        okay &= fseek(file, bipolar_min, SEEK_CUR) == -1 && errno == EINVAL;
        okay &= ftell(file) == 1 && fgetc(file) == 'b';
        okay &= fseek(file, bipolar_max, SEEK_SET) == 0 && fputc('z', file) == 'z';
        errno = 0;
        okay &= ftell(file) == -1 && errno == EOVERFLOW;
        fclose(file);
        return okay;
}

b32 main(void)
{
        if (program_argument_count() > 2)
                return injected_write_stop((b32)string_to_positive(program_argument(2)));
        if (program_argument_count() > 1)
                return body_allocation_failure();
        if (!empty_mode_stays_bounded() || !checked_write_errors() ||
            !bounded_line_reads() || !bounded_stream_positions())
                return 1;
        trace_body();
        bool fits = dynamic_buffer_fits_one_shelf();

        log_flush();
        return fits ? 0 : 1;
}
