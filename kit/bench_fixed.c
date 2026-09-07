#include "../src/compiler_memory.c"

static p8 records[65536];
static volatile positive observed;

/* Independent C carry loop, with identical complete-record boundaries. */
static __attribute__((noinline)) positive series_c(
    p8 address_to into, positive room, positive length,
    positive first, positive finish, bipolar step)
{
        positive used = length;
        while (room - used >= length)
        {
                memory_copy_apart(into + used, into + used - length, length);
                positive carry = step < 0 ? (positive)0 - (positive)step : (positive)step;
                for (positive at = finish; carry && at > first;)
                {
                        p8 address_to byte = into + used + --at;
                        if (*byte == '.') continue;
                        b32 digit = *byte - '0', part = carry % 10;
                        carry /= 10;
                        digit += step < 0 ? -part : part;
                        if (digit < 0) { digit += 10; carry++; }
                        if (digit >= 10) { digit -= 10; carry++; }
                        *byte = '0' + digit;
                }
                if (carry) break;
                used += length;
        }
        return used;
}

b32 main(void)
{
        const bipolar steps[] = {1, -1, 25, -25};
        for (positive s = 0; s < array_count(steps); s++)
                for (positive pass = 0; pass < 4; pass++)
                {
                        bool assembly = pass & 1;
                        positive started = get_cpu_time();
                        for (positive run = 0; run < 1000; run++)
                        {
                                memory_copy_apart(records, steps[s] < 0
                                    ? "9999999\n" : "0000000\n", 8);
                                positive used = assembly
                                    ? memory_decimal_series(records, sizeof(records), 8, 0, 7, steps[s])
                                    : series_c(records, sizeof(records), 8, 0, 7, steps[s]);
                                observed += used + records[used - 2];
                        }
                        positive elapsed = get_cpu_time() - started;
                        log(assembly ? "asm step " : "C step ", 0);
                        bipolar_to_string(log, steps[s]);
                        log(": ", 2);
                        positive_to_string(log, elapsed);
                        log(" ticks / 65536000 bytes\n", 0);
                }
        log_flush();
        return observed == 0;
}
