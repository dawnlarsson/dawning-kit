#ifndef DAWN_MODERN_C_PLATFORM_MACOS
#define DAWN_MODERN_C_PLATFORM_MACOS

#include "../library.c"

fn sleep(positive seconds, positive nanoseconds)
{
        return;
        // struct timespec time = {seconds, nanoseconds};
        // system_call_1(syscall(nanosleep), (positive)address_of time);
}

fn exit(b32 code)
{
        system_call_1(syscall(exit), code);
}

fn start()
{
        // register_get(stack_pointer, program_stack_base);

        b32 result = main();

        exit(result);
}

#endif