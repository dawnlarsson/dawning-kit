#include "../../standard/library.c"

#define label TERM_BOLD "[Init]" TERM_RESET " "
#define init_program "/shell"

b32 main()
{
        system_call(syscall(setsid));

        positive process_id = system_call(syscall(clone));

        if (process_id == 0)
        {
                while (1)
                        sleep(1, 0);
        }

        p8 address_to argv[] = {init_program};

        bipolar result = system_call_2(syscall(execve), (positive)init_program, (positive)argv);

        string_format(log, label "Failed to execute init program: %s  error: %b\n", init_program, result);

        log_flush();

        return 1;
}