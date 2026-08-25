/*
        Experimental C standard library

        macOS platform specific functions and definitions

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_MACOS
#define STANDARD_MODERN_C_PLATFORM_MACOS

#include "../library.c"

// XNU has no nanosleep syscall: libsystem builds it out of __semwait_signal.
// select with no descriptors and a timeout is a real syscall that sleeps for
// the requested interval, which is all this needs to do.
//
// The signature matches the fn sleep(timespec address_to) contract declared
// in library.c -- the platform files must agree on it.
fn sleep(timespec address_to time)
{
        struct
        {
                b64 tv_sec;
                b32 tv_usec;
        } timeout;

        timeout.tv_sec = (b64)time->tv_sec;
        timeout.tv_usec = (b32)(time->tv_nsec / 1000);

        system_call_5(syscall(select), 0, 0, 0, 0, (positive)address_of timeout);
}

fn exit(b32 code)
{
        system_call_1(syscall(exit), code);
}

// Named _start to match the linker entry point the build scripts pass with
// -Wl,-e,_start, and to match the linux platform file.
fn _start()
{
        register_get(stack_pointer, program_stack_base);

        b32 result = main();

        exit(result);
}

#endif
