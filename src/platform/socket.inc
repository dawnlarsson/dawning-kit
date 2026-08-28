/*
        Experimental C standard library

        Sockets, and the byte order the wire asks for

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_SOCKET
#define STANDARD_MODERN_C_PLATFORM_SOCKET

/*
        Byte order.

        A port number and an IPv4 address travel most significant byte first,
        which is the opposite of every machine this library targets, so the
        conversion sits on the path of every connection made and every packet
        addressed. It is one instruction on two of the three architectures and
        the compiler already knows which one:

                x86_64   bswap  %eax        rol $8, %ax
                arm64    rev    w0, w0      rev16 w0, w0

        __builtin_bswap32 emits exactly those, so wrapping them in hand
        written assembly would replace a single inlined instruction with a
        call and a return. The builtin is the floor there and is used as one.

        riscv64 is the interesting case. Reversing bytes is rev8, which lives
        in the Zbb extension, and the baseline this project builds for --
        rv64imafd_zicsr_zicntr -- does not have it. Asked for the swap anyway,
        gcc does not expand it inline at all: it emits a call to __bswapsi2 in
        libgcc. Every binary here links -nostdlib, so that is not merely a
        slow byte swap, it is an undefined reference and the link fails. The
        builtin is unusable on this target rather than just unattractive.

        Written as shifts and masks instead, gcc produces a correct sequence
        of twelve instructions. The one below is ten. Rotating the halves
        first lines both byte pairs up against the same mask, so 0x00FF00FF is
        materialised once and used twice, where the four-piece form gcc picks
        needs both 0x0000FF00 and 0x00FF0000 built separately:

                x = B0 B1 B2 B3   rotate 16 ->  B2 B3 B0 B1
                swapping the bytes inside each half of that is B3 B2 B1 B0

        The shifts are the w forms, so each one produces a value already sign
        extended from bit 31 and the result needs no widening step. Scratch
        registers are operands rather than named clobbers, which leaves the
        allocator free at the call site.

        Reversing is its own inverse, so one routine serves both directions
        and network_order names the intent without a second body. On a big
        endian host the conversion is nothing at all, and the reversal stays
        available under its own name for the callers that mean the bytes.
*/

#if RISCV64

static INLINE p32 bytes_reverse_32(p32 value)
{
        p32 out, part, mask;
        __asm__("slliw %0, %3, 16\n\t"
                "srliw %1, %3, 16\n\t"
                "or    %0, %0, %1\n\t"
                "lui   %2, 0xff0\n\t"
                "addi  %2, %2, 255\n\t"
                "and   %1, %0, %2\n\t"
                "slliw %1, %1, 8\n\t"
                "srliw %0, %0, 8\n\t"
                "and   %0, %0, %2\n\t"
                "or    %0, %0, %1"
                : "=&r"(out), "=&r"(part), "=&r"(mask)
                : "r"(value));
        return out;
}

static INLINE p16 bytes_reverse_16(p16 value)
{
        p16 out;
        p32 part;
        __asm__("srliw %0, %2, 8\n\t"
                "andi  %0, %0, 255\n\t"
                "slliw %1, %2, 8\n\t"
                "andi  %1, %1, -256\n\t"
                "or    %0, %0, %1"
                : "=&r"(out), "=&r"(part)
                : "r"(value));
        return out;
}

#else

static INLINE p32 bytes_reverse_32(p32 value) { return __builtin_bswap32(value); }
static INLINE p16 bytes_reverse_16(p16 value) { return __builtin_bswap16(value); }

#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define network_order_32(value) (value)
#define network_order_16(value) (value)
#else
#define network_order_32(value) bytes_reverse_32(value)
#define network_order_16(value) bytes_reverse_16(value)
#endif

/*
        The sockets themselves are Linux only, deliberately.

        syscall.inc carries Darwin's socket numbers and they are correct, but
        a sockaddr there is not the one below: Darwin spends the first byte on
        a length and leaves the family in the second, where Linux gives the
        family the full leading halfword. SOL_SOCKET and the option names
        differ as well. None of that is hard, but writing it here without a
        Darwin machine to run it on would be shipping a struct that is quietly
        the wrong shape, so this waits for someone who can test it.

        Everything above this point is arithmetic and builds everywhere.
*/
#ifdef LINUX

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SHUT_READ 0
#define SHUT_WRITE 1
#define SHUT_BOTH 2

#define NETLINK_ROUTE 0

#define HOST_ANY 0x00000000u
#define HOST_LOOPBACK 0x7f000001u
#define HOST_BROADCAST 0xffffffffu

/*
        The address structures, at the sizes the kernel checks.

        A socket call is handed the length as well as the pointer and the
        kernel compares it, so these are not merely descriptive: sockaddr_in
        is padded to sixteen bytes because that is what the kernel expects to
        be told, not because anything reads the padding. The port and the host
        are held in wire order, which is why they are written through
        network_order rather than assigned.
*/
typedef struct
{
        p16 family;
        p8 data[14];
} socket_address;

typedef struct
{
        p16 family;
        p16 port;
        p32 host;
        p8 padding[8];
} socket_address_internet;

typedef struct
{
        p16 family;
        p16 port;
        p32 flow;
        p8 host[16];
        p32 scope;
} socket_address_internet6;

typedef struct
{
        p16 family;
        p16 padding;
        p32 port;
        p32 groups;
} socket_address_netlink;

/*
        The calls.

        These stay C over system_call_N rather than becoming assembly bodies
        of their own. system_call_N is inline assembly the compiler drops at
        the call site, so socket_new below already compiles to argument moves
        and a trap with no call in it, and an ASM_FUNC spelling would add the
        call it is meant to avoid. The file routines are written out in
        assembly because they fuse work -- struct field stores around two
        traps -- and a three argument socket call has nothing to fuse. If a
        fused routine earns it later, the address setup and connect pair being
        the obvious candidate, it can be written then against a measurement.

        Every one of them returns what the kernel returned, negative errno and
        all, which is the convention the file routines already keep.
*/
static bipolar socket_new(b32 family, b32 kind, b32 protocol)
{
        return system_call_3(syscall(socket), (positive)family, (positive)kind,
                             (positive)protocol);
}

static bipolar socket_bind(b32 handle, address_any address, positive size)
{
        return system_call_3(syscall(bind), (positive)handle, (positive)address, size);
}

static bipolar socket_connect(b32 handle, address_any address, positive size)
{
        return system_call_3(syscall(connect), (positive)handle, (positive)address, size);
}

static bipolar socket_listen(b32 handle, b32 waiting)
{
        return system_call_2(syscall(listen), (positive)handle, (positive)waiting);
}

static bipolar socket_accept(b32 handle, address_any address, address_any size, b32 flags)
{
        return system_call_4(syscall(accept4), (positive)handle, (positive)address,
                             (positive)size, (positive)flags);
}

static bipolar socket_send(b32 handle, address_any data, positive size, b32 flags,
                           address_any to, positive to_size)
{
        return system_call_6(syscall(sendto), (positive)handle, (positive)data, size,
                             (positive)flags, (positive)to, to_size);
}

static bipolar socket_receive(b32 handle, address_any data, positive size, b32 flags,
                              address_any from, address_any from_size)
{
        return system_call_6(syscall(recvfrom), (positive)handle, (positive)data, size,
                             (positive)flags, (positive)from, (positive)from_size);
}

static bipolar socket_option_set(b32 handle, b32 level, b32 name, address_any value,
                                 positive size)
{
        return system_call_5(syscall(setsockopt), (positive)handle, (positive)level,
                             (positive)name, (positive)value, size);
}

static bipolar socket_option_get(b32 handle, b32 level, b32 name, address_any value,
                                 address_any size)
{
        return system_call_5(syscall(getsockopt), (positive)handle, (positive)level,
                             (positive)name, (positive)value, (positive)size);
}

static bipolar socket_name(b32 handle, address_any address, address_any size)
{
        return system_call_3(syscall(getsockname), (positive)handle, (positive)address,
                             (positive)size);
}

static bipolar socket_shutdown(b32 handle, b32 direction)
{
        return system_call_2(syscall(shutdown), (positive)handle, (positive)direction);
}

static bipolar socket_close(b32 handle)
{
        return system_call_1(syscall(close), (positive)handle);
}

#endif // LINUX

#endif // STANDARD_MODERN_C_PLATFORM_SOCKET
