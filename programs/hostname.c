#include "../src/sh/file.c"

// hostname, and hostname -s for the part before the first dot.
b32 main()
{
        file_machine facts;
        positive first = 0;
        positive flags = file_take_options((string_address) "sf", address_of first);

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
        {
                file_fail("hostname: cannot read system name\n", 0);
                return 1;
        }

        if (flags & FILE_FLAG('s'))
        {
                string_address dot = string_first_of(facts.node, '.');

                if (dot)
                        address_to dot = end;
        }

        file_line(facts.node);
        log_flush();

        return 0;
}
