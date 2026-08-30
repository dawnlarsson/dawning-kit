/* Minimal process-entry harness: stack, arguments, environment, and exit. */
#include "../src/compiler_memory.c"

b32 main(void)
{
        string_address address_to kernel_environment =
                program_environment_list();

        if (program_argument_count() < 1 || is_null(program_argument(0)))
                return 1;

        if (environ != kernel_environment)
                return 2;

        if (kernel_environment == null)
                return 3;

        return 0;
}
