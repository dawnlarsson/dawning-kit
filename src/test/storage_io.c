#include "../compiler_memory.c"
#include "counted.inc"

/* Inject only positional reads. Everything else reaches the real syscall
   ABI, so stdbuf can open/read/close a real ELF-header fixture while its
   out-of-line program table receives deterministic short reads and EINTR. */
static positive storage_test_mode, storage_test_calls;
static positive storage_test_offset, storage_test_length, storage_test_used;
static bool storage_test_arguments, storage_test_preclear;
static p8 address_to storage_test_destination;

static bipolar storage_test_call4(positive number, positive handle,
                                  positive into, positive length,
                                  positive offset)
{
        if (number != syscall(pread64) || !storage_test_mode)
                return (system_call_4)(number, handle, into, length, offset);

        p8 address_to bytes = (p8 address_to)into;
        if (!storage_test_calls && storage_test_mode != 5)
                for (positive i = 0; i < storage_test_length; i++)
                        if (storage_test_destination[i] != 0xa5)
                                storage_test_preclear = true;

        if (offset != storage_test_offset + storage_test_used ||
            length != storage_test_length - storage_test_used)
                storage_test_arguments = false;

        storage_test_calls++;
        if (storage_test_calls == 1 &&
            (storage_test_mode == 1 || storage_test_mode == 5))
                return -4;
        if (storage_test_mode == 3 ||
            (storage_test_mode == 4 && storage_test_used == 7))
                return -5;
        if (storage_test_mode == 2 && storage_test_used == 7)
                return 0;

        positive take = min(length, (positive)7);
        for (positive i = 0; i < take; i++)
                bytes[i] = storage_test_mode == 5
                               ? (storage_test_used + i == 0 ? 3 : 0)
                               : (p8)(storage_test_used + i + 1);
        storage_test_used += take;
        return (bipolar)take;
}

#define system_call_4(...) storage_test_call4(__VA_ARGS__)
#include "../spark.c"
#include "../sh/shell.c"
#undef system_call_4

static fn storage_test_begin(positive mode, positive length, positive offset,
                             p8 address_to bytes)
{
        storage_test_mode = mode;
        storage_test_calls = storage_test_used = 0;
        storage_test_offset = offset;
        storage_test_length = length;
        storage_test_arguments = true;
        storage_test_preclear = false;
        storage_test_destination = bytes;
}

static fn storage_test_read(positive mode, positive wanted)
{
        p8 guarded[34];
        memory_fill(guarded, 0xa5, sizeof guarded);
        storage_test_begin(mode, 32, 123, guarded + 1);
        positive got = storage_read(12345, guarded + 1, 32, 123);
        storage_test_mode = 0;

        check("positional read count", got == wanted);
        check("positional short reads advance pointer and offset",
              storage_test_arguments);
        check("destination guards", guarded[0] == 0xa5 && guarded[33] == 0xa5);
        check("full probe is not cleared before read", !storage_test_preclear);
        for (positive i = 0; i < 32; i++)
                check("read bytes survive and unread tail is zero",
                      guarded[i + 1] == (i < got ? (p8)(i + 1) : 0));
}

static fn storage_test_elf(void)
{
        bipolar handle = system_call_2(syscall(memfd_create),
                                        (positive)"storage-elf-test", 0);
        check("ELF fixture descriptor", handle >= 0);
        if (handle < 0)
                return;

        p8 head[64] = {0x7f, 'E', 'L', 'F', 2, 1};
        head[16] = 2;
        positive machine = stdbuf_elf_machine();
        head[18] = (p8)machine;
        head[19] = (p8)(machine >> 8);
        head[33] = 32; /* e_phoff = 8192, beyond the initial read. */
        head[54] = 56;
        head[56] = 1;
        check("ELF fixture written", system_write_all((positive)handle,
              head, sizeof head) == sizeof head);

        p8 path[64];
        memory_copy_apart(path, "/proc/self/fd/", 14);
        positive_into_string(path + 14, (positive)handle);
        storage_test_begin(5, 56, 8192, null);
        b32 kind = stdbuf_target_kind(path);
        storage_test_mode = 0;
        system_close((positive)handle);
        check("stdbuf retries interrupted and short ELF table reads",
              kind == STDBUF_ELF_DYNAMIC && storage_test_used == 56 &&
              storage_test_calls == 9 && storage_test_arguments);
}

b32 main(void)
{
        storage_test_read(1, 32);
        storage_test_read(2, 7);
        storage_test_read(3, 0);
        storage_test_read(4, 7);
        storage_test_elf();
        return test_report(null);
}
