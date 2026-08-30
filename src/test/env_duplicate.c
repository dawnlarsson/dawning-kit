#include "../compiler_memory.c"

/*
        execve's environment is a vector, not a map.  Host `env` utilities
        collapse repeated names before the shell can see them, so this tiny
        launcher is the only honest regression for duplicate initial-stack
        entries and their borrowed lifetime.
*/
b32 main()
{
        string_address shell = program_argument(1);
        string_address arguments[] = {
            shell,
            "-c",
            "printf '%s\\n' \"$MW_DUPLICATE\"; unset MW_DUPLICATE; "
            "MW_DUPLICATE=owned; printf '%s\\n' \"$MW_DUPLICATE\"",
            null};
        string_address environment[] = {
            "MW_DUPLICATE=first", "MW_DUPLICATE=last", null};

        if (!shell)
                return 2;

        system_call_3(syscall(execve), (positive)shell, (positive)arguments,
                      (positive)environment);
        return 126;
}
