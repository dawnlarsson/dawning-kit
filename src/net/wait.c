/*
        Experimental C standard library

        Waiting for a readable descriptor

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_WAIT
#define STANDARD_MODERN_C_NET_WAIT

#define NETWORK_INTERRUPTED (-4)

/* DHCP uses the same availability-oriented entropy policy as temporary-file
   nonces: nonblocking kernel randomness, then the kernel's early-boot byte
   stream, and finally a mixed timing/PID/ASLR fallback. The width guard stays
   here because callers copy only the low bytes into their wire field. DNS,
   whose 16-bit tag authenticates a remote reply, uses the strict helper
   below instead. */
static inline INLINE positive network_transaction(positive width)
{
        if (!width || width > sizeof(positive))
                return 0;

        return system_nonce();
}

/* A DNS id is part of reply authentication, so its availability tradeoff is
   stricter than DHCP's local-link tag and an exclusive temporary filename.
   Refuse to send when the kernel CSPRNG is not ready rather than exposing a
   timing/PID-derived 16-bit value. */
static inline INLINE bool network_transaction_secure(address_any into,
                                                      positive width)
{
        if (!into || !width || width > sizeof(positive))
                return false;

        return system_random_fill(into, width, 1) == 0;
}

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

/*
        A stream which has stopped making progress must eventually give its
        caller back control.  This is installed once, before connect, and
        Linux then applies the send timeout to connect and writes and the
        receive timeout to reads, without every TLS and HTTP loop growing its
        own timer state.

        This is an idle timeout rather than a limit on the size or duration of
        a transfer: every successful system call starts a fresh wait.  Large
        downloads therefore remain possible, while a peer which accepts a
        connection and says nothing cannot hold wget -- or its synchronous
        bowl caller -- forever.
*/
static bool network_stream_timeout(bipolar handle, positive seconds,
                                   positive microseconds)
{
        timeval limit;

        if ((!seconds && !microseconds) || microseconds >= 1000000)
                return false;

        limit.tv_sec = (b64)seconds;
        limit.tv_usec = (b64)microseconds;

        return socket_option_set((b32)handle, SOL_SOCKET, SO_RCVTIMEO,
                                 address_of limit, sizeof limit) >= 0 &&
               socket_option_set((b32)handle, SOL_SOCKET, SO_SNDTIMEO,
                                 address_of limit, sizeof limit) >= 0;
}

#endif
