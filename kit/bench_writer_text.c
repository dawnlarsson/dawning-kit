/* Buffered output policy: former C against the shared assembly core. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
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

NOT_INLINED static fn former_flush(positive handle, p8 address_to buffer,
                                   positive address_to count)
{
        if (!address_to count)
                return;

        system_write_all(handle, buffer, address_to count);
        address_to count = 0;
}

NOT_INLINED static fn former_put(positive handle, p8 address_to buffer,
                                 positive capacity, positive address_to count,
                                 address_any data, positive length, bool hold_equal)
{
        if (!length)
                return;

        if (length > capacity || (!hold_equal && length == capacity))
        {
                former_flush(handle, buffer, count);
                system_write_all(handle, data, length);
                return;
        }

        if (length > capacity - address_to count)
                former_flush(handle, buffer, count);

        memory_copy(buffer + address_to count, data, length);
        address_to count += length;
}

NOT_INLINED static fn former_byte(positive handle, p8 address_to buffer,
                                  positive capacity, positive address_to count,
                                  p8 byte)
{
        if (!capacity)
        {
                system_write_all(handle, address_of byte, 1);
                return;
        }

        if (address_to count >= capacity)
                former_flush(handle, buffer, count);

        buffer[address_to count] = byte;
        address_to count += 1;
}

static p64 put_once(positive length, bool hold_equal, bool assembly,
                    positive rounds, positive pending)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
        {
                used = pending;

                if (assembly)
                {
                        if (hold_equal)
                                buffered_write_deferred_equal(null_handle, output,
                                                              CAPACITY, address_of used,
                                                              payload, length);
                        else
                                buffered_write(null_handle, output, CAPACITY,
                                               address_of used, payload, length);
                }
                else
                        former_put(null_handle, output, CAPACITY, address_of used,
                                   payload, length, hold_equal);

                sink += used + output[0];
        }

        return get_cpu_time() - start;
}

static p64 byte_once(bool assembly)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < BYTE_ROUNDS; i++)
        {
                used = i & (CAPACITY - 1);

                if (assembly)
                        buffered_write_byte(null_handle, output, CAPACITY,
                                            address_of used, (p8)i);
                else
                        former_byte(null_handle, output, CAPACITY,
                                    address_of used, (p8)i);

                sink += used + output[i & (CAPACITY - 1)];
        }

        return get_cpu_time() - start;
}

static p64 flush_once(bool assembly)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < SYSCALL_ROUNDS; i++)
        {
                used = CAPACITY;

                if (assembly)
                        buffered_flush(null_handle, output, address_of used);
                else
                        former_flush(null_handle, output, address_of used);

                sink += used;
        }

        return get_cpu_time() - start;
}

static fn order(positive address_to ratios)
{
        for (positive i = 1; i < TRIES; i++)
        {
                positive value = ratios[i];
                positive at = i;

                while (at && ratios[at - 1] > value)
                {
                        ratios[at] = ratios[at - 1];
                        at--;
                }

                ratios[at] = value;
        }
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
        row((string_address)"byte", 0, false, 0, 0, true, false);
        row((string_address)"combined overflow", 8, false, SYSCALL_ROUNDS,
            CAPACITY - 4, false, false);
        row((string_address)"direct exact", CAPACITY, false, SYSCALL_ROUNDS,
            0, false, false);
        row((string_address)"hold exact", CAPACITY, true, SYSCALL_ROUNDS,
            0, false, false);
        row((string_address)"flush", 0, false, 0, 0, false, true);

        system_call_1(syscall(close), null_handle);
        log_flush();
        return 0;
}
