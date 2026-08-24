#include "../../standard/library.c"

// Reads its own environment back out of /proc, which is the only way to see
// what the spawner actually handed over. Entries are NUL separated.
p8 buffer[4096];

b32 main()
{
        bipolar file = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive) "/proc/self/environ", FILE_READ, 0);

        if (file < 0)
        {
                string_format(log, "envtest: cannot read /proc/self/environ: %b\n", file);
                log_flush();
                return 1;
        }

        bipolar length = system_call_3(syscall(read), file, (positive)buffer, sizeof(buffer) - 1);
        system_call_1(syscall(close), file);

        if (length <= 0)
        {
                string_format(log, "envtest: environment is empty\n");
                log_flush();
                return 1;
        }

        buffer[length] = 0;

        positive at = 0;
        while (at < (positive)length)
        {
                string_format(log, "  env: %s\n", buffer + at);
                at += string_length(buffer + at) + 1;
        }

        log_flush();
        return 0;
}
