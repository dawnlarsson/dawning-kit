#include "../compiler_memory.c"

/* The shared buffered-writer policy, including both exact-capacity modes. */

#ifndef LINUX
#error "writer-buffer syscall behavior is tested on Linux"
#endif

static positive checks;
static positive failures;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        string_format(log, "  FAIL " name "\n");        \
                }                                                       \
        } while (0)

static bool read_exact(positive handle, p8 address_to into, positive want)
{
        positive have = 0;

        while (have < want)
        {
                bipolar got = system_read_retry(handle, into + have, want - have);

                if (got <= 0)
                        return false;

                have += (positive)got;
        }

        return true;
}

static bool bytes_are(p8 address_to got, string_address want, positive length)
{
        return !memory_compare(got, want, length);
}

b32 main(void)
{
        b32 ends[2];
        p8 guarded[18];
        p8 address_to buffer = guarded + 1;
        p8 received[64];
        p8 exact[16] = "0123456789abcdef";
        p8 large[17] = "ABCDEFGHIJKLMNOPQ";
        positive used = 0;

        check("pipe made", system_call_2(syscall(pipe2), (positive)ends, 0) == 0);
        memory_fill(guarded, 0xa5, sizeof guarded);

        check("zero length succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             (address_any)"ignored", 0));
        check("zero length is a no-op", used == 0 && guarded[0] == 0xa5 &&
                                                guarded[17] == 0xa5);

        check("buffered run succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             (address_any)"abc", 3));
        check("small write is buffered", used == 3 && bytes_are(buffer, "abc", 3));

        check("exact fill succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             exact, 13));
        check("combined exact fill stays buffered", used == 16);
        check("combined exact bytes", bytes_are(buffer, "abc0123456789abc", 16));
        check("buffer guards survive", guarded[0] == 0xa5 && guarded[17] == 0xa5);

        check("byte after full buffer succeeds",
              buffered_write_byte((positive)ends[1], buffer, 16,
                                  address_of used, '!'));
        check("byte flushes a full buffer", used == 1 && buffer[0] == '!');
        check("full buffer arrived", read_exact((positive)ends[0], received, 16) &&
                                            bytes_are(received, "abc0123456789abc", 16));

        check("flush succeeds",
              buffered_flush((positive)ends[1], buffer, address_of used));
        check("flush clears pending count", used == 0);
        check("pending byte arrived", read_exact((positive)ends[0], received, 1) &&
                                          received[0] == '!');

        check("direct exact succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             exact, 16));
        check("at-capacity policy writes directly", used == 0);
        check("direct exact bytes", read_exact((positive)ends[0], received, 16) &&
                                       bytes_are(received, exact, 16));

        check("deferred exact succeeds",
              buffered_write_deferred_equal((positive)ends[1], buffer, 16,
                                            address_of used, exact, 16));
        check("hold-equal policy buffers exact", used == 16 &&
                                                  bytes_are(buffer, exact, 16));
        check("deferred flush succeeds",
              buffered_flush((positive)ends[1], buffer, address_of used));
        check("held exact bytes", read_exact((positive)ends[0], received, 16) &&
                                     bytes_are(received, exact, 16));

        check("direct prefix buffers",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             (address_any)"pre", 3));
        check("flush then direct succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             large, 17));
        check("over-capacity write leaves no pending bytes", used == 0);
        check("pending precedes direct bytes",
              read_exact((positive)ends[0], received, 20) &&
                  bytes_are(received, "preABCDEFGHIJKLMNOPQ", 20));

        check("unbuffered byte succeeds",
              buffered_write_byte((positive)ends[1], buffer, 0,
                                  address_of used, '?'));
        check("zero-capacity byte is unbuffered", used == 0 &&
                                                  read_exact((positive)ends[0], received, 1) &&
                                                  received[0] == '?');

        check("unbuffered run succeeds",
              buffered_write_deferred_equal((positive)ends[1], buffer, 0,
                                            address_of used,
                                            (address_any)"xy", 2));
        check("zero-capacity run is unbuffered", used == 0 &&
                                                 read_exact((positive)ends[0], received, 2) &&
                                                 bytes_are(received, "xy", 2));

        used = 3;
        memory_copy(buffer, "bad", 3);
        check("failed flush is reported",
              !buffered_flush((positive)-1, buffer, address_of used));
        check("failed flush still clears pending state", used == 0);

        check("failed direct run is reported",
              !buffered_write((positive)-1, buffer, 0, address_of used,
                              (address_any)"x", 1));
        check("failed direct byte is reported",
              !buffered_write_byte((positive)-1, buffer, 0,
                                   address_of used, 'x'));

        used = 3;
        memory_copy(buffer, "bad", 3);
        check("failed prefix flush stops a direct run",
              !buffered_write((positive)-1, buffer, 16, address_of used,
                              large, 17));
        check("failed prefix is discarded", used == 0);

        used = 16;
        memory_copy(buffer, exact, 16);
        check("failed full flush stops a byte",
              !buffered_write_byte((positive)-1, buffer, 16,
                                   address_of used, 'x'));
        check("failed full buffer is discarded", used == 0);
        check("final guards survive", guarded[0] == 0xa5 && guarded[17] == 0xa5);

        system_call_1(syscall(close), (positive)ends[0]);
        system_call_1(syscall(close), (positive)ends[1]);

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();
        return failures ? 1 : 0;
}
