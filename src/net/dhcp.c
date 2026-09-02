/*
        Experimental C standard library

        dhcp: an address obtained rather than typed

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_DHCP
#define STANDARD_MODERN_C_NET_DHCP

#include "wait.c"

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

/*
        One packet, built.

        The fixed part is 236 bytes of BOOTP with the hardware address in it,
        then the magic cookie that says the options which follow are DHCP's
        rather than BOOTP's, then the options themselves ending in 255.
*/
static positive dhcp_build(p8 address_to into, positive room, p8 kind, p32 transaction,
                           p8 address_to hardware, p32 wanted, p32 server, p32 holding)
{
        positive at;

        if (room < 300)
                return 0;

        memory_fill(into, 0, 300);

        into[0] = 1;   // a request, from a client
        into[1] = 1;   // over ethernet
        into[2] = 6;   // whose addresses are six bytes
        into[3] = 0;   // and which no relay has forwarded

        network_store_32(into + 4, transaction);

        //      Ask to be answered by broadcast, unless we are renewing: a
        //      client that already holds an address can be replied to
        //      directly, and asking for a broadcast then is noise on every
        //      other machine's wire.
        if (!holding)
        {
                into[10] = (p8)(DHCP_FLAG_BROADCAST >> 8);
                into[11] = 0;
        }

        //      ciaddr. Zero while asking for an address; the address we
        //      already hold while asking to keep it, which is what tells the
        //      server this is a renewal rather than a new client.
        network_store_32(into + 12, holding);

        memory_copy(into + 28, hardware, 6);

        network_store_32(into + DHCP_HEAD, DHCP_COOKIE);

        at = DHCP_HEAD + 4;

        into[at++] = DHCP_OPTION_TYPE;
        into[at++] = 1;
        into[at++] = kind;

        if (wanted)
        {
                into[at++] = DHCP_OPTION_REQUESTED;
                into[at++] = 4;
                network_store_32(into + at, wanted);
                at += 4;
        }

        if (server)
        {
                into[at++] = DHCP_OPTION_SERVER;
                into[at++] = 4;
                network_store_32(into + at, server);
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

        if (network_load_32(packet + 4) != transaction)
                return -1;

        if (memory_compare(packet + 28, hardware, 6))
                return -1;

        if (network_load_32(packet + DHCP_HEAD) != DHCP_COOKIE)
                return -1;

        lease->address = network_load_32(packet + 16);  // yiaddr
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
                                lease->mask = network_load_32(packet + at + 2);
                        break;
                case DHCP_OPTION_ROUTER:
                        if (length >= 4)
                                lease->router = network_load_32(packet + at + 2);
                        break;
                case DHCP_OPTION_DNS:
                        if (length >= 4)
                                lease->nameserver = network_load_32(packet + at + 2);
                        break;
                case DHCP_OPTION_SERVER:
                        if (length == 4)
                                lease->server = network_load_32(packet + at + 2);
                        break;
                case DHCP_OPTION_LEASE:
                        if (length == 4)
                                lease->seconds = network_load_32(packet + at + 2);
                        break;
                default:
                        break;
                }

                at += 2 + length;
        }

        return address_to kind ? 0 : -1;
}

//      A mask of n leading bits, said as the prefix length a route wants.
static CONST p8 dhcp_prefix_of(p32 mask)
{
        p32 first_zero = ~mask;
        p8 bits = first_zero
                    ? (p8)(bits_leading_zeros((positive)first_zero) - 32)
                    : 32;

        return bits ? bits : 24;
}

static bipolar dhcp_open(string_address device, p32 host, bool broadcast)
{
        bipolar handle = socket_new(AF_INET, SOCK_DGRAM, 0);
        b32 one = 1;

        if (handle < 0)
                return handle;

        socket_address_internet mine = {
            .family = AF_INET, .port = network_order_16(DHCP_CLIENT_PORT),
            .host = network_order_32(host)};

        if (broadcast)
                socket_option_set((b32)handle, SOL_SOCKET, SO_BROADCAST,
                                  address_of one, sizeof one);

        socket_option_set((b32)handle, SOL_SOCKET, SO_REUSEADDR,
                          address_of one, sizeof one);
        socket_option_set((b32)handle, SOL_SOCKET, SO_BINDTODEVICE, device,
                          string_length(device) + 1);

        if (socket_bind((b32)handle, address_of mine, sizeof mine) < 0)
        {
                socket_close((b32)handle);
                return -1;
        }

        return handle;
}

static bool dhcp_receive(bipolar handle, p8 address_to packet, positive room,
                         p32 transaction, p8 address_to hardware,
                         dhcp_lease address_to lease, p8 address_to kind,
                         positive seconds, positive nanoseconds)
{
        bipolar got;

        if (network_wait_readable(handle, seconds, nanoseconds) <= 0)
                return false;

        got = socket_receive((b32)handle, packet, room, 0, 0, 0);

        return got > 0 &&
               dhcp_read(packet, (positive)got, transaction, hardware,
                         lease, kind) >= 0;
}

/*
        The exchange, with a schedule rather than a single try.

        A server that is slow, or a link that has only just come up and whose
        switch port is still learning, is the ordinary case at boot rather
        than the exception. A link brought up a moment ago has not finished
        negotiating, the first DISCOVERs go into that gap and are simply lost,
        and what decides when a machine is on the network is how soon after
        carrier the next one goes out. So it asks four times a second for the
        first three seconds and backs off after that.

        A note on where the time went, because it was not where it looked.

        A boot reached carrier at three seconds and had no address until
        fifteen. Counting showed the whole exchange was one send, one poll,
        one offer and one poll for the acknowledgement -- perhaps two seconds
        of work -- inside fourteen seconds of wall clock, and a direct test
        showed ppoll honouring its timeout to the millisecond.

        It was getrandom. With no flags it waits for the kernel's entropy pool
        to be initialised, and early in boot it is not. Twelve seconds of a
        boot were spent there, before a single packet moved, asking for a
        number to put in a header. GRND_NONBLOCK asks not to wait, and the
        clock answers instead when the pool will not.

        Three seconds to carrier, four to an address. The second of those is
        qemu, not this.
*/
static bipolar dhcp_ask(string_address device, p8 address_to hardware,
                        dhcp_lease address_to lease)
{
        p8 packet[1024];
        p32 transaction;
        bipolar handle;
        positive length;
        positive attempt;
        positive wait;

        memory_fill(lease, 0, sizeof(dhcp_lease));
        transaction = (p32)network_transaction(sizeof transaction);
        handle = dhcp_open(device, HOST_ANY, true);

        if (handle < 0)
                return DHCP_NO_SOCKET;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT),
            .host = network_order_32(HOST_BROADCAST)};

        for (attempt = 0; attempt < 20; attempt++)
        {
                p8 kind = 0;
                positive deadline;

                /*
                        A quarter second apart while it matters.

                        The link comes up about three seconds into a boot and
                        the DISCOVERs before that are lost, so what decides
                        when a machine is on the network is how soon after
                        carrier the next one goes out. At one second that was
                        the whole of the remaining delay; at a quarter it is
                        within noise of the card itself.

                        Twelve quick tries covers three seconds of that, and
                        the backoff after it is for a network with no server
                        on it, which should not be broadcast at forever.
                */
                wait = attempt < 12 ? 1 : (attempt - 11) * 8;
                deadline = wait;

                length = dhcp_build(packet, sizeof packet, DHCP_DISCOVER, transaction,
                                    hardware, 0, 0, 0);

                if (socket_send((b32)handle, packet, length, 0, address_of where,
                                sizeof where) < 0)
                {
                        //      No route yet, most likely. An unconnected UDP
                        //      socket routes every send afresh, so carrier can
                        //      appear underneath this one before the next try.
                        continue;
                }

                while (deadline--)
                {
                        if (!dhcp_receive(handle, packet, sizeof packet, transaction,
                                          hardware, lease, address_of kind,
                                          0, 250000000))
                                continue;

                        if (kind != DHCP_OFFER)
                                continue;

                        //      Take the offer, naming the server so that any
                        //      other server that offered knows it lost.
                        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST,
                                            transaction, hardware, lease->address,
                                            lease->server, 0);

                        socket_send((b32)handle, packet, length, 0, address_of where,
                                    sizeof where);

                        deadline = wait;

                        while (deadline--)
                        {
                                if (!dhcp_receive(handle, packet, sizeof packet,
                                                  transaction, hardware, lease,
                                                  address_of kind, 0, 250000000))
                                        continue;

                                if (kind == DHCP_ACK || kind == DHCP_NAK)
                                {
                                        socket_close((b32)handle);
                                        return kind == DHCP_ACK ? DHCP_OK : DHCP_REFUSED;
                                }
                        }

                        break;
                }
        }

        socket_close((b32)handle);
        return DHCP_NO_OFFER;
}

/*
        Keeping the address we already have.

        A lease is a loan with a time on it. Half way through, a client is
        supposed to ask to keep what it has -- unicast to the server that gave
        it, with ciaddr set to the address and no server identifier, which is
        what distinguishes "may I keep this" from "may I have one". The server
        answers with an ACK and a fresh lease time.

        This matters more than it looks on a machine that stays up. qemu hands
        out a lease measured in days and nothing here would ever notice, but a
        home router giving an hour means a machine that has been up since
        yesterday is holding an address the server considers free, and the
        first thing that goes wrong is somebody else being given it.

        Renewing rather than starting over is the whole point: a fresh
        DISCOVER may come back with a different address, and every connection
        open at the time dies with it.

        A failure here is not fatal and not reported as one. The caller falls
        back to asking from scratch, which is what a client does when the
        lease finally expires anyway.
*/
static bipolar dhcp_renew(string_address device, p8 address_to hardware,
                          dhcp_lease address_to lease)
{
        p8 packet[1024];
        p32 transaction;
        dhcp_lease fresh;
        bipolar handle;
        positive length;
        positive deadline;
        p8 kind = 0;

        if (!lease->address || !lease->server)
                return DHCP_NO_OFFER;

        transaction = (p32)network_transaction(sizeof transaction);
        handle = dhcp_open(device, lease->address, false);

        if (handle < 0)
                return DHCP_NO_SOCKET;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT),
            .host = network_order_32(lease->server)};

        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST, transaction,
                            hardware, 0, 0, lease->address);

        if (socket_send((b32)handle, packet, length, 0, address_of where,
                        sizeof where) < 0)
        {
                socket_close((b32)handle);
                return DHCP_NO_SOCKET;
        }

        for (deadline = 0; deadline < 4; deadline++)
        {
                memory_fill(address_of fresh, 0, sizeof fresh);

                if (!dhcp_receive(handle, packet, sizeof packet, transaction,
                                  hardware, address_of fresh, address_of kind,
                                  1, 0))
                        continue;

                if (kind == DHCP_ACK && fresh.address == lease->address)
                {
                        //      Keep what the renewal said, including the new
                        //      lease time, but do not lose what it left out:
                        //      an ACK need not repeat every option.
                        if (fresh.mask)
                                lease->mask = fresh.mask;

                        if (fresh.router)
                                lease->router = fresh.router;

                        if (fresh.nameserver)
                                lease->nameserver = fresh.nameserver;

                        if (fresh.server)
                                lease->server = fresh.server;

                        lease->seconds = fresh.seconds;

                        socket_close((b32)handle);

                        return DHCP_OK;
                }

                if (kind == DHCP_NAK)
                {
                        socket_close((b32)handle);
                        return DHCP_REFUSED;
                }
        }

        socket_close((b32)handle);

        return DHCP_NO_OFFER;
}

#endif // STANDARD_MODERN_C_NET_DHCP
