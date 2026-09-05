/* Real production reader, real warm memfd reads. Only reads are timed;
   file creation, initial touching, formatting and validation are outside.
   Usage: storage-read [bytes 1..65536] [iterations 1..1000000]. */
#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

b32 main(void)
{
        positive size = 4096, loops = 262144;
        if (program_argument_count() > 1)
                size = string_to_number_unsigned(program_argument(1), null, 10);
        if (program_argument_count() > 2)
                loops = string_to_number_unsigned(program_argument(2), null, 10);
        if (!size || size > 65536 || !loops || loops > 1000000)
                return 2;
        bipolar handle = system_call_2(syscall(memfd_create),
                                        (positive)"storage-read-benchmark", 0);
        if (handle < 0)
                return 1;
        p8 bytes[65536];
        memory_fill(bytes, 0x5a, size);
        if (system_write_all((positive)handle, bytes, size) != size ||
            storage_read(handle, bytes, size, 0) != size)
                return 1;

        positive total = 0;
        p64 start = get_cpu_time();
        for (positive i = 0; i < loops; i++)
                total += storage_read(handle, bytes, size, 0);
        p64 ticks = get_cpu_time() - start;
        bool valid = total == loops * size &&
                     bytes[0] == 0x5a && bytes[size - 1] == 0x5a;
        system_close((positive)handle);
        string_format(log,
                      "storage-read bytes=%p loops=%p ticks=%p valid=%p\n",
                      size, loops, ticks, (positive)valid);
        return valid ? 0 : 1;
}
