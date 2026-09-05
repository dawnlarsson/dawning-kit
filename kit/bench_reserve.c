/* Run a fresh process per sample: ru_maxrss is a lifetime high-water mark.
   Usage: reserve [MiB, 1..128] [sparse]. Only growth is timed; allocation and
   initial touching precede it. Virtual capacity is not counted as RSS. */
#include "../src/compiler_memory.c"

b32 main(void)
{
        positive mib = 64;
        if (program_argument_count() > 1)
                mib = string_to_number_unsigned(program_argument(1), null, 10);
        if (!mib || mib > 128)
                return 2;
        bool sparse = program_argument_count() > 2 &&
                !string_compare(program_argument(2), (string_address)"sparse");
        positive have = mib * 1024 * 1024, used = have;
        address_any held = memory(have);
        if (!held || (positive)held >= (positive)-4095)
                return 1;
        if (sparse)
                system_call_3(syscall(madvise), (positive)held, have, 15);
        if (!sparse)
                memory_fill(held, 0x5a, have);
        ((p8 address_to)held)[0] = 0x3e;
        ((p8 address_to)held)[used - 1] = 0x7a;

        p64 started = get_cpu_time();
        bool grown = memory_reserve(address_of held, address_of have,
                                    used, used + 1, 1, 64);
        p64 ticks = get_cpu_time() - started;
        positive usage[18] = {0};
        bipolar measured = system_call_2(syscall(getrusage), 0, (positive)usage);
        bool valid = grown && ((p8 address_to)held)[0] == 0x3e &&
                ((p8 address_to)held)[used - 1] == 0x7a;
        string_format(log,
                "reserve mib=%p sparse=%p ticks=%p peak_rss_kib=%p minor_faults=%p valid=%p\n",
                mib, (positive)sparse, ticks, usage[4], usage[8], (positive)valid);
        memory_release(address_of held, address_of have, address_of used, 1);
        return valid && measured == 0 ? 0 : 1;
}
