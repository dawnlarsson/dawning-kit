/*
        Experimental C standard library

        dhcp: an address obtained rather than typed

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_DHCP
#define STANDARD_MODERN_C_NET_DHCP

/*
        The four messages, and the chicken and egg underneath them.

        A machine asking for an address has no address, so it cannot bind a
        socket to one and the server cannot route a reply back to it. DHCP
        works around that by broadcasting in both directions: the client sends
        from 0.0.0.0:68 to 255.255.255.255:67, and the server answers to the
        broadcast address, with the hardware address in the packet being the
        only thing that says which machine it is for.

        Two details make the difference between this working and this looking
        like a network fault:

        SO_BINDTODEVICE. With no address configured there is no route, so a
        send to 255.255.255.255 has no interface to leave by and fails with
        ENETUNREACH. Naming the interface on the socket is what supplies the
        answer the routing table cannot.

        The broadcast flag. A server may unicast its reply to an address the
        client does not have yet, which some clients receive anyway and some
        do not. Setting the flag asks for a broadcast reply, which always
        arrives. qemu's user-mode network broadcasts regardless and echoes the
        flag back as zero, so this is the one part of the exchange that the
        boot lane structurally cannot check -- it is set because a real
        network needs it, not because a test proved it here.

        What is deliberately not here: the ARP probe that checks nobody else
        is already using the offered address, and the T1 renewal timer. Both
        matter on a long-lived machine and neither is needed to get on the
        network, so they are absent rather than half done.
*/

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_HEAD 236
#define DHCP_COOKIE 0x63825363

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6

#define DHCP_OPTION_PAD 0
#define DHCP_OPTION_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS 6
#define DHCP_OPTION_REQUESTED 50
#define DHCP_OPTION_LEASE 51
#define DHCP_OPTION_TYPE 53
#define DHCP_OPTION_SERVER 54
#define DHCP_OPTION_ASK 55
#define DHCP_OPTION_END 255

#define DHCP_FLAG_BROADCAST 0x8000

#define DHCP_OK 0
#define DHCP_NO_SOCKET (-1)
#define DHCP_NO_OFFER (-2)
#define DHCP_REFUSED (-3)

typedef struct
{
        p32 address;
        p32 mask;
        p32 router;
        p32 nameserver;
        p32 server;
        p32 seconds;
} dhcp_lease;

//      Every multi-byte field in this packet is big endian, like the wire and
//      unlike netlink.
static fn dhcp_write_32(p8 address_to at, p32 value)
{
        at[0] = (p8)(value >> 24);
        at[1] = (p8)(value >> 16);
        at[2] = (p8)(value >> 8);
        at[3] = (p8)value;
}

static p32 dhcp_read_32(p8 address_to at)
{
        return ((p32)at[0] << 24) | ((p32)at[1] << 16) | ((p32)at[2] << 8) | at[3];
}

/*
        One packet, built.

        The fixed part is 236 bytes of BOOTP with the hardware address in it,
        then the magic cookie that says the options which follow are DHCP's
        rather than BOOTP's, then the options themselves ending in 255.
*/
static positive dhcp_build(p8 address_to into, positive room, p8 kind, p32 transaction,
                           p8 address_to hardware, p32 wanted, p32 server)
{
        positive at;

        if (room < 300)
                return 0;

        memory_fill(into, 0, 300);

        into[0] = 1;   // a request, from a client
        into[1] = 1;   // over ethernet
        into[2] = 6;   // whose addresses are six bytes
        into[3] = 0;   // and which no relay has forwarded

        dhcp_write_32(into + 4, transaction);

        //      Ask to be answered by broadcast: there is no address yet for a
        //      unicast reply to be addressed to.
        into[10] = (p8)(DHCP_FLAG_BROADCAST >> 8);
        into[11] = 0;

        memory_copy(into + 28, hardware, 6);

        dhcp_write_32(into + DHCP_HEAD, DHCP_COOKIE);

        at = DHCP_HEAD + 4;

        into[at++] = DHCP_OPTION_TYPE;
        into[at++] = 1;
        into[at++] = kind;

        if (wanted)
        {
                into[at++] = DHCP_OPTION_REQUESTED;
                into[at++] = 4;
                dhcp_write_32(into + at, wanted);
                at += 4;
        }

        if (server)
        {
                into[at++] = DHCP_OPTION_SERVER;
                into[at++] = 4;
                dhcp_write_32(into + at, server);
                at += 4;
        }

        //      What we would like to be told, which a server may ignore.
        into[at++] = DHCP_OPTION_ASK;
        into[at++] = 3;
        into[at++] = DHCP_OPTION_MASK;
        into[at++] = DHCP_OPTION_ROUTER;
        into[at++] = DHCP_OPTION_DNS;

        into[at++] = DHCP_OPTION_END;

        //      Short packets are dropped by some servers and by some switches,
        //      so it is padded to the length everything accepts.
        return at > 300 ? at : 300;
}

/*
        A reply read for what it says.

        Options are walked rather than indexed: a server sends what it likes
        in whatever order, and the length byte is the only thing that says
        where the next one starts. A length that would run off the end is a
        corrupt packet and ends the walk rather than reading past it.
*/
static bipolar dhcp_read(p8 address_to packet, positive size, p32 transaction,
                         p8 address_to hardware, dhcp_lease address_to lease,
                         p8 address_to kind)
{
        positive at = DHCP_HEAD + 4;

        if (size < DHCP_HEAD + 4)
                return -1;

        if (packet[0] != 2)  // not a reply
                return -1;

        if (dhcp_read_32(packet + 4) != transaction)
                return -1;

        if (memory_compare(packet + 28, hardware, 6))
                return -1;

        if (dhcp_read_32(packet + DHCP_HEAD) != DHCP_COOKIE)
                return -1;

        lease->address = dhcp_read_32(packet + 16);  // yiaddr
        address_to kind = 0;

        while (at < size)
        {
                p8 option = packet[at];
                p8 length;

                if (option == DHCP_OPTION_END)
                        break;

                if (option == DHCP_OPTION_PAD)
                {
                        at++;
                        continue;
                }

                if (at + 1 >= size)
                        return -1;

                length = packet[at + 1];

                if (at + 2 + length > size)
                        return -1;

                switch (option)
                {
                case DHCP_OPTION_TYPE:
                        if (length == 1)
                                address_to kind = packet[at + 2];
                        break;
                case DHCP_OPTION_MASK:
                        if (length == 4)
                                lease->mask = dhcp_read_32(packet + at + 2);
                        break;
                case DHCP_OPTION_ROUTER:
                        if (length >= 4)
                                lease->router = dhcp_read_32(packet + at + 2);
                        break;
                case DHCP_OPTION_DNS:
                        if (length >= 4)
                                lease->nameserver = dhcp_read_32(packet + at + 2);
                        break;
                case DHCP_OPTION_SERVER:
                        if (length == 4)
                                lease->server = dhcp_read_32(packet + at + 2);
                        break;
                case DHCP_OPTION_LEASE:
                        if (length == 4)
                                lease->seconds = dhcp_read_32(packet + at + 2);
                        break;
                default:
                        break;
                }

                at += 2 + length;
        }

        return address_to kind ? 0 : -1;
}

//      A mask of n leading bits, said as the prefix length a route wants.
static p8 dhcp_prefix_of(p32 mask)
{
        p8 bits = 0;

        while (mask & 0x80000000u)
        {
                bits++;
                mask <<= 1;
        }

        return bits ? bits : 24;
}

/*
        The exchange, with a schedule rather than a single try.

        A server that is slow, or a link that has only just come up and whose
        switch port is still learning, is the ordinary case at boot rather
        than the exception. So the wait doubles: four seconds, then eight,
        then sixteen. Giving up after one silent second is what makes a
        machine that would have worked look like one that cannot.
*/
static bipolar dhcp_ask(string_address device, p8 address_to hardware,
                        dhcp_lease address_to lease)
{
        socket_address_internet where;
        p8 packet[1024];
        p32 transaction;
        bipolar handle;
        positive length;
        b32 one = 1;
        positive wait;

        memory_fill(lease, 0, sizeof(dhcp_lease));

        if (system_call_3(syscall(getrandom), (positive)address_of transaction,
                          sizeof transaction, 0) != sizeof transaction)
                transaction = (p32)get_cpu_time();

        handle = socket_new(AF_INET, SOCK_DGRAM, 0);

        if (handle < 0)
                return DHCP_NO_SOCKET;

        socket_option_set((b32)handle, SOL_SOCKET, SO_BROADCAST, address_of one,
                          sizeof one);
        socket_option_set((b32)handle, SOL_SOCKET, SO_REUSEADDR, address_of one,
                          sizeof one);

        //      Without this the send has no route and fails: there is no
        //      address yet, so nothing in the routing table can carry it.
        socket_option_set((b32)handle, SOL_SOCKET, SO_BINDTODEVICE, device,
                          string_length(device) + 1);

        memory_fill(address_of where, 0, sizeof where);
        where.family = AF_INET;
        where.port = network_order_16(DHCP_CLIENT_PORT);
        where.host = network_order_32(HOST_ANY);

        if (socket_bind((b32)handle, address_of where, sizeof where) < 0)
        {
                socket_close((b32)handle);
                return DHCP_NO_SOCKET;
        }

        memory_fill(address_of where, 0, sizeof where);
        where.family = AF_INET;
        where.port = network_order_16(DHCP_SERVER_PORT);
        where.host = network_order_32(HOST_BROADCAST);

        for (wait = 4; wait <= 16; wait += wait)
        {
                p8 kind = 0;
                positive deadline = wait;

                length = dhcp_build(packet, sizeof packet, DHCP_DISCOVER, transaction,
                                    hardware, 0, 0);

                if (socket_send((b32)handle, packet, length, 0, address_of where,
                                sizeof where) < 0)
                {
                        socket_close((b32)handle);
                        return DHCP_NO_SOCKET;
                }

                while (deadline--)
                {
                        p8 waited[8];
                        timespec limit;
                        bipolar ready;
                        bipolar got;

                        address_to((b32 address_to)waited) = (b32)handle;
                        address_to((p16 address_to)(waited + 4)) = 1;
                        address_to((p16 address_to)(waited + 6)) = 0;

                        limit.tv_sec = 1;
                        limit.tv_nsec = 0;

                        ready = system_call_5(syscall(ppoll), (positive)waited, 1,
                                              (positive)address_of limit, 0, 8);

                        if (ready <= 0)
                                continue;

                        got = socket_receive((b32)handle, packet, sizeof packet, 0, 0, 0);

                        if (got <= 0)
                                continue;

                        if (dhcp_read(packet, (positive)got, transaction, hardware,
                                      lease, address_of kind) < 0)
                                continue;

                        if (kind != DHCP_OFFER)
                                continue;

                        //      Take the offer, naming the server so that any
                        //      other server that offered knows it lost.
                        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST,
                                            transaction, hardware, lease->address,
                                            lease->server);

                        socket_send((b32)handle, packet, length, 0, address_of where,
                                    sizeof where);

                        deadline = wait;

                        while (deadline--)
                        {
                                limit.tv_sec = 1;
                                limit.tv_nsec = 0;

                                address_to((b32 address_to)waited) = (b32)handle;
                                address_to((p16 address_to)(waited + 4)) = 1;

                                if (system_call_5(syscall(ppoll), (positive)waited, 1,
                                                  (positive)address_of limit, 0, 8) <= 0)
                                        continue;

                                got = socket_receive((b32)handle, packet, sizeof packet,
                                                     0, 0, 0);

                                if (got <= 0)
                                        continue;

                                if (dhcp_read(packet, (positive)got, transaction,
                                              hardware, lease, address_of kind) < 0)
                                        continue;

                                if (kind == DHCP_ACK)
                                {
                                        socket_close((b32)handle);
                                        return DHCP_OK;
                                }

                                if (kind == DHCP_NAK)
                                {
                                        socket_close((b32)handle);
                                        return DHCP_REFUSED;
                                }
                        }

                        break;
                }
        }

        socket_close((b32)handle);

        return DHCP_NO_OFFER;
}

#endif // STANDARD_MODERN_C_NET_DHCP
