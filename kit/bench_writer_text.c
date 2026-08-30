/* Buffered output policy: former C against the shared assembly core. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

// Keep the former general-purpose body general. GCC otherwise clones a copy
// for the constant capacity in each row and times that specialised copy
// against the public assembly ABI, which is not the implementation replaced.
#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 9
#define BUFFER_ROUNDS (1u << 18)
#define BYTE_ROUNDS (1u << 20)
#define SYSCALL_ROUNDS 2048
#define CAPACITY 4096

static p8 output[CAPACITY];
static p8 payload[CAPACITY * 2];
static positive used;
static positive null_handle;
static volatile positive sink;
static volatile positive benchmark_capacity = CAPACITY;
static volatile positive benchmark_reserve_length = 16;

NOT_INLINED static b32 former_flush(positive handle, p8 address_to buffer,
                                    positive address_to count)
{
        if (!address_to count)
                return true;

        positive wanted = address_to count;
        positive written = system_write_all(handle, buffer, wanted);
        address_to count = 0;
        return written == wanted;
}

NOT_INLINED static b32 former_put(positive handle, p8 address_to buffer,
                                  positive capacity, positive address_to count,
                                  address_any data, positive length, bool hold_equal)
{
        if (!length)
                return true;

        if (length > capacity || (!hold_equal && length == capacity))
        {
                if (!former_flush(handle, buffer, count))
                        return false;

                return system_write_all(handle, data, length) == length;
        }

        if (length > capacity - address_to count &&
            !former_flush(handle, buffer, count))
                return false;

        memory_copy(buffer + address_to count, data, length);
        address_to count += length;
        return true;
}

NOT_INLINED static p8 address_to former_reserve(
    positive handle, p8 address_to buffer, positive capacity,
    positive address_to count, positive length)
{
        if (length > capacity)
                return null;

        if (length > capacity - address_to count &&
            !former_flush(handle, buffer, count))
                return null;

        p8 address_to answer = buffer + address_to count;
        address_to count += length;
        return answer;
}

// Hot no-flush traffic floor: the caller-proven span needs one old-count load,
// one new-count store and one returned address. It deliberately omits the
// capacity and flush semantics the public routine must retain.
NOT_INLINED static p8 address_to reserve_floor(p8 address_to buffer,
                                               positive address_to count,
                                               positive length)
{
        p8 address_to answer = buffer + address_to count;
        address_to count += length;
        return answer;
}

NOT_INLINED static b32 former_byte(positive handle, p8 address_to buffer,
                                   positive capacity, positive address_to count,
                                   p8 byte)
{
        if (!capacity)
                return system_write_all(handle, address_of byte, 1) == 1;

        if (address_to count >= capacity &&
            !former_flush(handle, buffer, count))
                return false;

        buffer[address_to count] = byte;
        address_to count += 1;
        return true;
}

NOT_INLINED static fn former_log(address_any data, positive length)
{
        if (!length)
                length = string_length(data);

        /* The replaced logger was exactly this adapter with a tail call into
           the already-assembly buffered core.  Keep that core on both sides
           so this row isolates sticky-status overhead rather than comparing
           two different buffered writers. */
        (fn)buffered_write(1, log_writer_buffer, CAPACITY,
                          address_of log_writer_buffer_length, data, length);
}

static p64 put_once(positive length, bool hold_equal, bool assembly,
                    positive rounds, positive pending)
{
        positive capacity = benchmark_capacity;
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
        {
                b32 success;
                used = pending;

                if (assembly)
                {
                        if (hold_equal)
                                success = buffered_write_deferred_equal(
                                    null_handle, output, capacity, address_of used,
                                    payload, length);
                        else
                                success = buffered_write(null_handle, output, capacity,
                                                         address_of used, payload, length);
                }
                else
                        success = former_put(null_handle, output, capacity,
                                             address_of used, payload, length,
                                             hold_equal);

                sink += used + output[0] + (positive)success;
        }

        return get_cpu_time() - start;
}

static p64 byte_once(bool assembly)
{
        positive capacity = benchmark_capacity;
        p64 start = get_cpu_time();

        for (positive i = 0; i < BYTE_ROUNDS; i++)
        {
                b32 success;
                used = i & (capacity - 1);

                if (assembly)
                        success = buffered_write_byte(null_handle, output, capacity,
                                                      address_of used, (p8)i);
                else
                        success = former_byte(null_handle, output, capacity,
                                              address_of used, (p8)i);

                sink += used + output[i & (capacity - 1)] + (positive)success;
        }

        return get_cpu_time() - start;
}

static p64 reserve_once(b32 which)
{
        positive capacity = benchmark_capacity;
        positive length = benchmark_reserve_length;
        p64 start = get_cpu_time();

        for (positive i = 0; i < BUFFER_ROUNDS; i++)
        {
                // Keep this row at the no-syscall semantic floor. Flush and
                // failure paths have their own rows and focused tests.
                used = i & 2047;
                p8 address_to at;

                if (which == 1)
                        at = buffered_reserve(null_handle, output, capacity,
                                              address_of used, length);
                else if (which == 2)
                        at = reserve_floor(output, address_of used, length);
                else
                        at = former_reserve(null_handle, output, capacity,
                                            address_of used, length);

                at[0] = (p8)i;
                at[length - 1] = (p8)(i >> 8);
                sink += used + at[0] + at[length - 1];
        }

        return get_cpu_time() - start;
}

static p64 flush_once(bool assembly)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < SYSCALL_ROUNDS; i++)
        {
                b32 success;
                used = CAPACITY;

                if (assembly)
                        success = buffered_flush(null_handle, output, address_of used);
                else
                        success = former_flush(null_handle, output, address_of used);

                sink += used + (positive)success;
        }

        return get_cpu_time() - start;
}

static p64 log_once(positive length, bool assembly)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < BUFFER_ROUNDS; i++)
        {
                log_writer_buffer_length = 0;

                if (assembly)
                        log(payload, length);
                else
                        former_log(payload, length);

                sink += log_writer_buffer_length + log_writer_buffer[0];
        }

        return get_cpu_time() - start;
}

static fn row(string_address name, positive length, bool hold_equal,
              positive rounds, positive pending, bool byte, bool flush)
{
        positive ratios[TRIES];

        for (positive t = 0; t < TRIES; t++)
        {
                p64 former;
                p64 assembly;

                if (t & 1)
                {
                        assembly = byte ? byte_once(true)
                                        : (flush ? flush_once(true)
                                                 : put_once(length, hold_equal, true,
                                                            rounds, pending));
                        former = byte ? byte_once(false)
                                      : (flush ? flush_once(false)
                                               : put_once(length, hold_equal, false,
                                                          rounds, pending));
                }
                else
                {
                        former = byte ? byte_once(false)
                                      : (flush ? flush_once(false)
                                               : put_once(length, hold_equal, false,
                                                          rounds, pending));
                        assembly = byte ? byte_once(true)
                                        : (flush ? flush_once(true)
                                                 : put_once(length, hold_equal, true,
                                                            rounds, pending));
                }

                ratios[t] = (positive)(assembly * 10000 / (former ? former : 1));
        }

        order(ratios);
        string_format(log, "  %s  median asm/C %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

static fn log_row(string_address name, positive length)
{
        positive ratios[TRIES];

        /* Preserve the report accumulated by the non-logger rows before the
           benchmark deliberately reuses the process log buffer as scratch. */
        log_flush();

        for (positive t = 0; t < TRIES; t++)
        {
                p64 former;
                p64 assembly;

                if (t & 1)
                {
                        assembly = log_once(length, true);
                        former = log_once(length, false);
                }
                else
                {
                        former = log_once(length, false);
                        assembly = log_once(length, true);
                }

                ratios[t] = (positive)(assembly * 10000 / (former ? former : 1));
        }

        order(ratios);
        log_writer_buffer_length = 0;
        string_format(log, "  %s  median current/former %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

static fn reserve_row()
{
        positive ratios[TRIES];
        positive assembly_each[TRIES];
        positive floor_each[TRIES];

        for (positive t = 0; t < TRIES; t++)
        {
                p64 former;
                p64 assembly;
                p64 floor;

                if (t & 1)
                {
                        assembly = reserve_once(1);
                        floor = reserve_once(2);
                        former = reserve_once(0);
                }
                else
                {
                        former = reserve_once(0);
                        floor = reserve_once(2);
                        assembly = reserve_once(1);
                }

                ratios[t] = (positive)(assembly * 10000 / (former ? former : 1));
                assembly_each[t] = (positive)(assembly * 100 / BUFFER_ROUNDS);
                floor_each[t] = (positive)(floor * 100 / BUFFER_ROUNDS);
        }

        order(ratios);
        order(assembly_each, TRIES);
        order(floor_each, TRIES);
        string_format(log, "  reserve 16  median asm/C %p.%p%%\n",
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
        positive assembly = assembly_each[TRIES / 2];
        positive floor = floor_each[TRIES / 2];
        if (assembly >= floor)
                string_format(log,
                              "              asm %p.%p cycles/op, no-flush floor %p.%p, gap %p.%p\n",
                              assembly / 100, assembly % 100,
                              floor / 100, floor % 100,
                              (assembly - floor) / 100,
                              (assembly - floor) % 100);
        else
                string_format(log,
                              "              asm %p.%p cycles/op beat the %p.%p floor proxy; unresolved\n",
                              assembly / 100, assembly % 100,
                              floor / 100, floor % 100);
}

b32 main(void)
{
        for (positive i = 0; i < sizeof(payload); i++)
                payload[i] = (p8)(i * 37 + 11);

        null_handle = (positive)system_call_4(syscall(openat),
                                               (positive)(bipolar)AT_FDCWD,
                                               (positive)"/dev/null", 1, 0);

        string_format(log, "buffered writer, paired median of %p\n", (positive)TRIES);
        row((string_address)"buffer 8", 8, false, BUFFER_ROUNDS, 0, false, false);
        row((string_address)"buffer 64", 64, false, BUFFER_ROUNDS, 0, false, false);
        reserve_row();
        row((string_address)"byte", 0, false, 0, 0, true, false);
        row((string_address)"combined overflow", 8, false, SYSCALL_ROUNDS,
            CAPACITY - 4, false, false);
        row((string_address)"direct exact", CAPACITY, false, SYSCALL_ROUNDS,
            0, false, false);
        row((string_address)"hold exact", CAPACITY, true, SYSCALL_ROUNDS,
            0, false, false);
        row((string_address)"flush", 0, false, 0, 0, false, true);
        log_row((string_address)"log buffer 8", 8);
        log_row((string_address)"log buffer 64", 64);

        system_call_1(syscall(close), null_handle);
        log_flush();
        return 0;
}
