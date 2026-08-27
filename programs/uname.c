#include "../src/sh/file.c"

/*
        uname [-asnrvmpio]

        -a is every field the kernel actually keeps. The system's own uname
        adds a compiled in operating system name after them, which is not in
        struct utsname and is not ours to claim, so -o names this system and
        -a stops at the machine.
*/
b32 main()
{
        file_machine facts;
        positive first = 0;
        positive flags = file_take_options((string_address) "asnrvmpio", address_of first);

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
        {
                file_fail("uname: cannot read system name\n", 0);
                return 1;
        }

        bool all = (flags & FILE_FLAG('a')) != 0;
        bool any = flags != 0;
        positive written = 0;

        if (all || (flags & FILE_FLAG('s')) || !any)
        {
                log(facts.system, 0);
                written++;
        }

        if (all || (flags & FILE_FLAG('n')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.node, 0);
        }

        if (all || (flags & FILE_FLAG('r')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.release, 0);
        }

        if (all || (flags & FILE_FLAG('v')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.version, 0);
        }

        if (all || (flags & FILE_FLAG('m')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.machine, 0);
        }

        if (flags & FILE_FLAG('p'))
        {
                if (written++)
                        log(" ", 1);

                log(facts.machine, 0);
        }

        if (flags & FILE_FLAG('i'))
        {
                if (written++)
                        log(" ", 1);

                log("unknown", 0);
        }

        if (flags & FILE_FLAG('o'))
        {
                if (written++)
                        log(" ", 1);

                log("Moonwater", 0);
        }

        log("\n", 1);
        log_flush();

        return 0;
}
