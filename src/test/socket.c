#include "../compiler_memory.c"
/*
        Byte order, and the socket calls that need it.

        The reversal is checked against a reference that pulls the bytes out
        one at a time and rebuilds them in the other order, which shares no
        code with any of the three implementations. Sixteen bits is small
        enough to check completely. Thirty two is walked over every single bit
        and its complement, the values that have caught this kind of routine
        before, and a stride that does not divide any power of two -- the
        interesting failures are sign extension on riscv64, where the shifts
        are the only thing keeping bit 31 from spreading upward, so the values
        with that bit set are the ones written out by hand.

        The socket half opens a real listening socket on loopback, lets the
        kernel choose the port, connects to it and moves bytes through. That
        is the part a compile cannot check: whether socket_address_internet is
        the shape the kernel expects to be handed. getsockname is asked for
        the address back, and both the family and the host are compared
        against what was put in, so a struct with a field in the wrong place
        fails here rather than the first time a route is added.
*/

static positive checks = 0;
static positive failures = 0;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        string_format(log, "  FAIL " name "\n");        \
                }                                                       \
        } while (0)

//      Independent of every reversal path: index the bytes, rebuild reversed.
static p32 reference_32(p32 value)
{
        p8 b0 = value & 0xff, b1 = (value >> 8) & 0xff;
        p8 b2 = (value >> 16) & 0xff, b3 = (value >> 24) & 0xff;
        return ((p32)b0 << 24) | ((p32)b1 << 16) | ((p32)b2 << 8) | (p32)b3;
}

static p16 reference_16(p16 value)
{
        p8 b0 = value & 0xff, b1 = (value >> 8) & 0xff;
        return (p16)(((p16)b0 << 8) | (p16)b1);
}

static fn byte_order(void)
{
        static const p32 awkward[] = {
                0, 1, 0x80000000u, 0xffffffffu, 0x7fffffffu, 0xdeadbeefu,
                0x00ff00ffu, 0xff00ff00u, 0x12345678u, 0x000000ffu, 0xff000000u,
        };

        for (p32 value = 0; value <= 0xffffu; value++)
                check("bytes_reverse_16", bytes_reverse_16((p16)value) ==
                                                  reference_16((p16)value));

        for (positive i = 0; i < sizeof awkward / sizeof *awkward; i++)
                check("bytes_reverse_32 awkward",
                      bytes_reverse_32(awkward[i]) == reference_32(awkward[i]));

        for (b32 bit = 0; bit < 32; bit++) {
                p32 value = 1u << bit;
                check("bytes_reverse_32 single bit",
                      bytes_reverse_32(value) == reference_32(value));
                check("bytes_reverse_32 single bit clear",
                      bytes_reverse_32(~value) == reference_32(~value));
        }

        for (p32 value = 0; value < 0x04000000u; value += 9973u)
                check("bytes_reverse_32 stride",
                      bytes_reverse_32(value) == reference_32(value));

        //      Reversing twice is the identity, which the wire relies on.
        for (p32 value = 0; value < 0x00400000u; value += 7919u)
                check("bytes_reverse_32 is its own inverse",
                      bytes_reverse_32(bytes_reverse_32(value)) == value);
}

/*
        The dotted quad, both ways.

        host_into does not terminate what it writes -- it returns a length, the
        way positive_into does -- so the round trip has to put the zero on
        itself before handing the buffer back. That is the interesting case
        anyway: every value written must read back as itself, which catches a
        formatter that drops a leading digit and a parser that accepts one.
*/
static const struct { const char *text; b64 expect; } address_cases[] = {
        {"0.0.0.0",            0x00000000},
        {"1.2.3.4",            0x01020304},
        {"127.0.0.1",          0x7f000001},
        {"192.168.1.10",       0xc0a8010a},
        {"255.255.255.255",    0xffffffff},
        {"10.0.0.255",         0x0a0000ff},
        {"9.9.9.9",            0x09090909},
        {"099.9.9.9",          -1},  // a leading zero, which iproute2 would read as octal
        {"010.1.1.1",          -1},
        {"01.2.3.4",           -1},
        {"1.2.3.04",           -1},
        {"00.0.0.0",           -1},
        {"0.0.0.0",            0x00000000},  // a single zero is still a zero
        {"",                   -1},
        {"1",                  -1},
        {"1.2",                -1},
        {"1.2.3",              -1},
        {"1.2.3.4.5",          -1},
        {"1.2.3.",             -1},
        {".1.2.3",             -1},
        {"1..2.3",             -1},
        {"256.1.1.1",          -1},
        {"1.256.1.1",          -1},
        {"1.1.1.256",          -1},
        {"1234.1.1.1",         -1},  // a fourth digit
        {"1.2.3.4 ",           -1},
        {" 1.2.3.4",           -1},
        {"1.2.3.4x",           -1},
        {"-1.2.3.4",           -1},
        {"1.2.3.-4",           -1},
        {"...",                -1},
        {"999.999.999.999",    -1},
};

static fn addresses(void)
{
        p8 scratch[32];

        for (positive i = 0; i < sizeof address_cases / sizeof *address_cases; i++)
                check("string_to_host",
                      string_to_host((string_address)address_cases[i].text) ==
                              address_cases[i].expect);

        //      What it writes, exactly, for the shapes that differ in width.
        check("host_into 0.0.0.0", host_into(scratch, 0x00000000) == 7);
        check("host_into 0.0.0.0 bytes", memory_compare(scratch, "0.0.0.0", 7) == 0);
        check("host_into 127.0.0.1", host_into(scratch, 0x7f000001) == 9);
        check("host_into 127.0.0.1 bytes", memory_compare(scratch, "127.0.0.1", 9) == 0);
        check("host_into 255.255.255.255", host_into(scratch, 0xffffffffu) == 15);
        check("host_into 255.255.255.255 bytes",
              memory_compare(scratch, "255.255.255.255", 15) == 0);
        check("host_into 8.8.8.8", host_into(scratch, 0x08080808) == 7);
        check("host_into 10.1.100.9", host_into(scratch, 0x0a016409) == 10);
        check("host_into 10.1.100.9 bytes",
              memory_compare(scratch, "10.1.100.9", 10) == 0);

        //      Every value written must read back as itself.
        for (p32 value = 0; value < 0x04000000u; value += 7919u) {
                positive width = host_into(scratch, value);
                scratch[width] = 0;
                check("host_into then string_to_host", string_to_host(scratch) == (b64)value);
                check("host_into width is sane", width >= 7 && width <= 15);
        }

        //      The boundaries of each octet's digit count, walked exactly.
        for (p32 octet = 0; octet < 256; octet++) {
                p32 value = (octet << 24) | (octet << 16) | (octet << 8) | octet;
                positive width = host_into(scratch, value);
                scratch[width] = 0;
                check("repeated octet round trip", string_to_host(scratch) == (b64)value);
        }
}

#ifdef LINUX
static fn sockets(void)
{
        socket_address_internet where, target, peer;
        p8 note[] = "hardware floor";
        p8 back[32];
        p32 size = sizeof where;
        b32 one = 1;

        bipolar server = socket_new(AF_INET, SOCK_STREAM, 0);
        check("socket_new stream", server >= 0);
        if (server < 0)
                return;

        check("socket_option_set", socket_option_set((b32)server, SOL_SOCKET,
                                                     SO_REUSEADDR, &one,
                                                     sizeof one) == 0);

        //      Both pointer arguments reach the kernel after the option name.
        //      This is also the call the original test never exercised at all.
        b32 option = 0;
        p32 option_size = sizeof option;
        check("socket_option_get", socket_option_get((b32)server, SOL_SOCKET,
                                                     SO_REUSEADDR, &option,
                                                     &option_size) == 0);
        check("socket_option_get size", option_size == sizeof option);
        check("socket_option_get value", option == one);

        memory_fill(&where, 0, sizeof where);
        where.family = AF_INET;
        where.port = network_order_16(0);
        where.host = network_order_32(HOST_LOOPBACK);
        check("socket_bind", socket_bind((b32)server, &where, sizeof where) == 0);
        check("socket_listen", socket_listen((b32)server, 4) == 0);

        //      The kernel writing the address back is what pins the struct.
        memory_fill(&where, 0, sizeof where);
        check("socket_name", socket_name((b32)server, &where, &size) == 0);
        check("socket_name filled the whole address", size == sizeof where);
        check("family came back", where.family == AF_INET);
        check("host came back in wire order",
              where.host == network_order_32(HOST_LOOPBACK));
        check("a port was chosen", network_order_16(where.port) != 0);

        bipolar client = socket_new(AF_INET, SOCK_STREAM, 0);
        check("socket_new client", client >= 0);

        memory_fill(&target, 0, sizeof target);
        target.family = AF_INET;
        target.port = where.port;
        target.host = network_order_32(HOST_LOOPBACK);
        check("socket_connect",
              socket_connect((b32)client, &target, sizeof target) == 0);

        p32 peer_size = sizeof peer;
        memory_fill(&peer, 0, sizeof peer);
        bipolar accepted = socket_accept((b32)server, &peer, &peer_size,
                                         SOCK_CLOEXEC);
        check("socket_accept", accepted >= 0);
        check("socket_accept address size", accepted >= 0 &&
                                            peer_size == sizeof peer);
        check("socket_accept address family", accepted >= 0 &&
                                              peer.family == AF_INET);

        //      F_GETFD is one and FD_CLOEXEC is bit zero on Linux. A successful
        //      check proves accept4 saw its nonzero fourth argument, including
        //      x86_64's rcx-to-r10 move.
        check("socket_accept flags", accepted >= 0 &&
              (system_call_3(syscall(fcntl), (positive)accepted, 1, 0) & 1));

        check("socket_send",
              socket_send((b32)client, note, sizeof note - 1, 0, 0, 0) ==
                    sizeof note - 1);
        memory_fill(back, 0, sizeof back);
        check("socket_receive",
              socket_receive((b32)accepted, back, sizeof back, 0, 0, 0) ==
                    sizeof note - 1);
        check("the bytes arrived unchanged",
              memory_compare(back, note, sizeof note - 1) == 0);

        check("socket_shutdown",
              socket_shutdown((b32)client, SHUT_BOTH) == 0);
        check("socket_close", socket_close((b32)client) == 0);
        socket_close((b32)accepted);
        socket_close((b32)server);

        /*
                A datagram is deliberately not connected. sendto therefore
                needs arguments five and six, and recvfrom writes through
                arguments five and six; MSG_DONTWAIT makes argument four
                nonzero on both. The stream round trip above could succeed if
                every one of those arguments were dropped.
        */
        socket_address_internet datagram, from;
        p8 packet[] = "all six arguments";
        p32 datagram_size = sizeof datagram;
        p32 from_size = sizeof from;
        bipolar receiver = socket_new(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bipolar sender = socket_new(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        check("socket_new datagram receiver", receiver >= 0);
        check("socket_new datagram sender", sender >= 0);

        memory_fill(&datagram, 0, sizeof datagram);
        datagram.family = AF_INET;
        datagram.host = network_order_32(HOST_LOOPBACK);
        check("socket_bind datagram", receiver >= 0 &&
              socket_bind((b32)receiver, &datagram, sizeof datagram) == 0);

        memory_fill(&datagram, 0, sizeof datagram);
        check("socket_name datagram", receiver >= 0 &&
              socket_name((b32)receiver, &datagram, &datagram_size) == 0);
        check("socket_name datagram size", datagram_size == sizeof datagram);
        check("socket_name datagram port", network_order_16(datagram.port) != 0);

        check("socket_send all arguments", sender >= 0 &&
              socket_send((b32)sender, packet, sizeof packet - 1, MSG_DONTWAIT,
                          &datagram, sizeof datagram) == sizeof packet - 1);

        memory_fill(back, 0, sizeof back);
        memory_fill(&from, 0, sizeof from);
        check("socket_receive all arguments", receiver >= 0 &&
              socket_receive((b32)receiver, back, sizeof back, MSG_DONTWAIT,
                             &from, &from_size) == sizeof packet - 1);
        check("datagram bytes arrived unchanged",
              memory_compare(back, packet, sizeof packet - 1) == 0);
        check("socket_receive address size", from_size == sizeof from);
        check("socket_receive address family", from.family == AF_INET);

        if (sender >= 0)
                socket_close((b32)sender);
        if (receiver >= 0)
                socket_close((b32)receiver);

        //      What address configuration will actually be talking to.
        socket_address_netlink self;
        bipolar route = socket_new(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
        check("socket_new netlink", route >= 0);
        memory_fill(&self, 0, sizeof self);
        self.family = AF_NETLINK;
        check("socket_bind netlink",
              socket_bind((b32)route, &self, sizeof self) == 0);
        socket_close((b32)route);

        //      A failure has to arrive as a negative errno, not as a trap.
        check("a bad handle is -EBADF", socket_listen(-1, 1) == -9);
}
#endif

b32 main(void)
{
        byte_order();
        addresses();
#ifdef LINUX
        sockets();
#endif
        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();
        return failures ? 1 : 0;
}
