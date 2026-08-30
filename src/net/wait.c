/*
        Experimental C standard library

        Waiting for a readable descriptor

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_WAIT
#define STANDARD_MODERN_C_NET_WAIT

/*
        ppoll rather than poll, because arm64 and riscv64 have only ppoll in
        the asm-generic syscall table. Keep the kernel's pollfd layout typed:
        writing one through casts into an eight-byte character array gives
        that array neither the alignment nor the effective type of the words
        being stored.
*/
static bipolar descriptor_wait_readable(bipolar handle,
                                         timespec address_to limit,
                                         positive address_to signal_mask)
{
        struct
        {
                b32 descriptor;
                b16 events;
                b16 returned;
        } waited = {(b32)handle, 1, 0};        // POLLIN

        return system_call_5(syscall(ppoll), (positive)address_of waited, 1,
                             (positive)limit, (positive)signal_mask, 8);
}

static bipolar network_wait_readable(bipolar handle, positive seconds,
                                     positive nanoseconds)
{
        timespec limit = {seconds, nanoseconds};

        return descriptor_wait_readable(handle, address_of limit, null);
}

#endif
