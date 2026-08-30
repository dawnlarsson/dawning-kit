#include "../compiler_memory.c"

/* The shared buffered-writer policy, including both exact-capacity modes. */

#ifndef LINUX
#error "writer-buffer syscall behavior is tested on Linux"
#endif

#include "counted.inc"

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
        p8 address_to reserved;
        p8 received[64];
        p8 exact[16] = "0123456789abcdef";
        p8 large[17] = "ABCDEFGHIJKLMNOPQ";
        positive used = 0;
        bipolar saved_output;
        bool deferred_not_failed;
        bool flush_failed;
        bool direct_failed;
        bool large_failed;
        bool failure_stuck;

        check("pipe made", system_call_2(syscall(pipe2), (positive)ends, 0) == 0);
        memory_fill(guarded, 0xa5, sizeof guarded);

        check("zero length succeeds",
              buffered_write((positive)ends[1], buffer, 16, address_of used,
                             (address_any)"ignored", 0));
        check("zero length is a no-op", used == 0 && guarded[0] == 0xa5 &&
                                                guarded[17] == 0xa5);

        reserved = buffered_reserve((positive)ends[1], buffer, 16,
                                    address_of used, 3);
        check("reserve answers its span", reserved == buffer && used == 3);
        memory_copy(reserved, "abc", 3);
        reserved = buffered_reserve((positive)ends[1], buffer, 16,
                                    address_of used, 13);
        check("reserve exact remainder stays buffered",
              reserved == buffer + 3 && used == 16);
        memory_copy(reserved, "0123456789abc", 13);
        reserved = buffered_reserve((positive)ends[1], buffer, 16,
                                    address_of used, 1);
        check("reserve flushes before a new span", reserved == buffer && used == 1);
        reserved[0] = '!';
        check("reserved full buffer arrived",
              read_exact((positive)ends[0], received, 16) &&
                  bytes_are(received, "abc0123456789abc", 16));
        check("reserved tail flush succeeds",
              buffered_flush((positive)ends[1], buffer, address_of used));
        check("reserved tail arrived",
              read_exact((positive)ends[0], received, 1) && received[0] == '!');

        used = 3;
        memory_copy(buffer, "pre", 3);
        check("over-capacity reserve is refused",
              !buffered_reserve((positive)ends[1], buffer, 16,
                                address_of used, 17));
        check("refused reserve leaves pending bytes", used == 3 &&
                                                        bytes_are(buffer, "pre", 3));
        check("pending bytes after refusal flush",
              buffered_flush((positive)ends[1], buffer, address_of used) &&
                  read_exact((positive)ends[0], received, 3) &&
                  bytes_are(received, "pre", 3));

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

        used = 16;
        memory_copy(buffer, exact, 16);
        check("failed reserve flush is reported",
              !buffered_reserve((positive)-1, buffer, 16,
                                address_of used, 1));
        check("failed reserve clears pending state", used == 0);

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

        /*
                The process logger has a void writer ABI.  Its separate
                sticky bit carries an output error across that ABI until a
                command boundary consumes it.  A pipe's read end is a
                deterministic unwritable stdout on every Linux architecture.
        */
        log_flush();
        saved_output = system_call_1(syscall(dup), 1);
        check("stdout saved for logger failure checks", saved_output >= 0);
        check("unwritable stdout installed",
              system_call_3(syscall(dup3), (positive)ends[0], 1, 0) == 1);

        log_failure_reset();
        log("x", 1);
        deferred_not_failed = !log_failed();
        log_flush();
        flush_failed = log_failed() && log_writer_buffer_length == 0;

        log_failure_reset();
        log_direct("x", 1);
        direct_failed = log_failed();

        log_failure_reset();
        log(log_writer_buffer, MAX_INPUT);
        large_failed = log_failed() && log_writer_buffer_length == 0;
        failure_stuck = log_failed() && log_failed();

        if (saved_output >= 0)
                system_call_3(syscall(dup3), (positive)saved_output, 1, 0);
        log_failure_reset();

        check("buffered logger defers failure until flush", deferred_not_failed);
        check("logger flush records failure and clears pending bytes", flush_failed);
        check("direct logger records failure", direct_failed);
        check("capacity-sized logger write records immediate failure", large_failed);
        check("logger failure is sticky until reset", failure_stuck);
        check("logger reset clears failure", !log_failed());

        if (saved_output >= 0)
                system_call_1(syscall(close), (positive)saved_output);

        system_call_1(syscall(close), (positive)ends[0]);
        system_call_1(syscall(close), (positive)ends[1]);

        return test_report(null);
}
