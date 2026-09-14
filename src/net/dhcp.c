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

        ARP conflict probing is not implemented. The network watcher schedules
        dhcp_renew at half the lease lifetime.
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
#define DHCP_OPTION_RENEWAL 58
#define DHCP_OPTION_REBINDING 59
#define DHCP_OPTION_END 255

#define DHCP_FLAG_BROADCAST 0x8000

#define DHCP_OK 0
#define DHCP_NO_SOCKET (-1)
#define DHCP_NO_OFFER (-2)
#define DHCP_REFUSED (-3)
#define DHCP_NO_RANDOM (-4)

typedef struct
{
        p32 address;
        p32 mask;
        p32 router;
        p32 nameserver;
        p32 server;
        p32 seconds;
        p32 renewal;
        p32 rebinding;
} dhcp_lease;

/* A transaction id is visible beside the client's public hardware address and
   is the only unpredictable field an off-path reply must guess.  Prefer the
   initialized CSPRNG.  Early boot still needs DHCP before that pool is ready,
   so retain Linux's explicit GRND_INSECURE stream; unlike system_nonce(), do
   not fall through to a timing/PID/ASLR value if both kernel entropy policies
   are unavailable. */
static bool dhcp_transaction_early(p32 address_to transaction)
{
        return network_transaction_secure(transaction, sizeof(*transaction)) ||
               system_random_fill(transaction, sizeof(*transaction), 4) == 0;
}

/* A subnet mask is a run of one bits followed by a run of zero bits.  Zero is
   retained as the existing "server omitted it" /24 policy; any other broken
   shape would silently configure a different network from the one offered. */
static CONST bool dhcp_mask_valid(p32 mask)
{
        p32 after = ~mask;

        return !mask || !(after & (after + 1));
}

/* All destinations are actual p32 fields. Address comes from the fixed
   header; the remaining fields are options, some containing address lists. */
static const struct { p8 option, offset; bool multiple; } dhcp_fields[] = {
    {0, __builtin_offsetof(dhcp_lease, address), false},
    {DHCP_OPTION_MASK, __builtin_offsetof(dhcp_lease, mask), false},
    {DHCP_OPTION_ROUTER, __builtin_offsetof(dhcp_lease, router), true},
    {DHCP_OPTION_DNS, __builtin_offsetof(dhcp_lease, nameserver), true},
    {DHCP_OPTION_SERVER, __builtin_offsetof(dhcp_lease, server), false},
    {DHCP_OPTION_LEASE, __builtin_offsetof(dhcp_lease, seconds), false},
    {DHCP_OPTION_RENEWAL, __builtin_offsetof(dhcp_lease, renewal), false},
    {DHCP_OPTION_REBINDING, __builtin_offsetof(dhcp_lease, rebinding), false},
};

/* RFC 2131 requires T1 < T2 < expiry.  Each omitted timer gets its standard
   default (one half and seven eighths of the lease); a supplied pair which
   breaks the ordering is discarded as a pair rather than creating a state
   machine which can skip RENEWING or outlive the lease. */
static bool dhcp_lease_timers(dhcp_lease address_to lease)
{
        p32 default_renewal;
        p32 default_rebinding;
        p32 renewal;
        p32 rebinding;

        if (!lease || !lease->seconds)
                return false;

        /* A one- or two-second lease is legal, but there are not enough whole
           seconds to encode both strict state boundaries.  Keep the lease and
           let the watcher wake only for its immediate expiry. */
        if (lease->seconds < 3)
        {
                lease->renewal = 0;
                lease->rebinding = 0;
                return true;
        }

        default_renewal = lease->seconds / 2;
        default_rebinding = lease->seconds -
                            (lease->seconds / 8 +
                             (lease->seconds % 8 != 0));
        renewal = lease->renewal ? lease->renewal : default_renewal;
        rebinding = lease->rebinding ? lease->rebinding : default_rebinding;

        if (!renewal || renewal >= rebinding ||
            rebinding >= lease->seconds)
        {
                renewal = default_renewal;
                rebinding = default_rebinding;
        }

        lease->renewal = renewal;
        lease->rebinding = rebinding;
        return renewal && renewal < rebinding && rebinding < lease->seconds;
}

/*
        One packet, built.

        The fixed part is 236 bytes of BOOTP with the hardware address in it,
        then the magic cookie that says the options which follow are DHCP's
        rather than BOOTP's, then the options themselves ending in 255.
*/
static positive dhcp_build(p8 address_to into, positive room, p8 kind,
                           p32 transaction, p8 address_to hardware, p32 wanted,
                           p32 server, p32 holding, bool broadcast)
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
        if (broadcast)
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
        dhcp_lease parsed = {0};
        positive at = DHCP_HEAD + 4;
        p8 parsed_kind = 0;

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

        parsed.address = network_load_32(packet + 16);  // yiaddr

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

                if (option == DHCP_OPTION_TYPE && length == 1)
                        parsed_kind = packet[at + 2];
                for (positive i = 1; i < array_count(dhcp_fields); i++)
                        if (option == dhcp_fields[i].option && length >= 4 &&
                            (length == 4 || dhcp_fields[i].multiple))
                                *(p32 *)((p8 *)&parsed + dhcp_fields[i].offset) =
                                    network_load_32(packet + at + 2);

                at += 2 + length;
        }

        if (!parsed_kind || !dhcp_mask_valid(parsed.mask))
                return -1;

        *lease = parsed;
        address_to kind = parsed_kind;

        return 0;
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

/* A DHCPACK is allowed to omit options already supplied by its offer.  Packet
   parsing itself stays replacement-based so unrelated packets cannot bleed
   into one another; only the stateful exchange chooses to retain an earlier
   nonzero field. */
static fn dhcp_lease_merge(dhcp_lease address_to lease,
                           const dhcp_lease address_to fresh)
{
        for (positive i = 0; i < array_count(dhcp_fields); i++)
        {
                p32 value = *(const p32 *)((const p8 *)fresh + dhcp_fields[i].offset);
                if (value)
                        *(p32 *)((p8 *)lease + dhcp_fields[i].offset) = value;
        }
}

static bool dhcp_lease_usable(const dhcp_lease address_to lease)
{
        return lease->address && lease->server && lease->seconds &&
               dhcp_mask_valid(lease->mask);
}

/* Every ACK starts a new lease interval.  Timer values from the OFFER or the
   preceding lease are relative to that older interval and cannot be inherited
   when the ACK omits options 58/59, especially when option 51 changed. */
static bool dhcp_lease_acknowledge(dhcp_lease address_to lease,
                                   const dhcp_lease address_to answer)
{
        lease->renewal = 0;
        lease->rebinding = 0;
        dhcp_lease_merge(lease, answer);
        return dhcp_lease_usable(lease) && dhcp_lease_timers(lease);
}

/* OFFER, ACK and NAK all carry a mandatory server identifier.  Once an
   OFFER has been selected, only that server may complete or refuse the
   exchange; xid and chaddr identify the client, not the selected server. */
static bool dhcp_answer_matches(p8 kind, const dhcp_lease address_to answer,
                                p32 selected_server)
{
        return (kind == DHCP_ACK || kind == DHCP_NAK) && selected_server &&
               answer->server == selected_server;
}

/* An acknowledgement completes the exact offer the client requested.  A NAK
   has no address to match, but still has to come from the selected server. */
static bool dhcp_acquisition_answer_matches(
    p8 kind, const dhcp_lease address_to answer,
    const dhcp_lease address_to offer)
{
        return dhcp_answer_matches(kind, answer, offer->server) &&
               (kind != DHCP_ACK || answer->address == offer->address);
}

/* RENEWING remains bound to the original server.  REBINDING deliberately
   accepts an authoritative answer from any server, but an ACK must still name
   the address already in use and every ACK/NAK must identify its server. */
static bool dhcp_reacquisition_answer_matches(
    p8 kind, const dhcp_lease address_to answer,
    const dhcp_lease address_to lease, bool rebinding)
{
        if (!answer || !lease || !answer->server ||
            (kind != DHCP_ACK && kind != DHCP_NAK))
                return false;
        if (!rebinding && answer->server != lease->server)
                return false;
        return kind != DHCP_ACK || answer->address == lease->address;
}

static bipolar dhcp_open(string_address device, p32 host, bool broadcast)
{
        bipolar handle = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        b32 one = 1;

        if (handle < 0)
                return handle;

        socket_address_internet mine = {
            .family = AF_INET, .port = network_order_16(DHCP_CLIENT_PORT),
            .host = network_order_32(host)};

        if ((broadcast && socket_option_set((b32)handle, SOL_SOCKET, SO_BROADCAST,
                                            address_of one, sizeof one) < 0) ||
            socket_option_set((b32)handle, SOL_SOCKET, SO_REUSEADDR,
                              address_of one, sizeof one) < 0 ||
            socket_option_set((b32)handle, SOL_SOCKET, SO_BINDTODEVICE, device,
                              string_length(device) + 1) < 0 ||
            socket_bind((b32)handle, address_of mine, sizeof mine) < 0)
        {
                socket_close((b32)handle);
                return -1;
        }

        return handle;
}

/* Packet fields identify the DHCP transaction; the datagram endpoint says
   who supplied them.  Every accepted reply comes from server port 67.
   Acquisition explicitly permits any OFFER host, whose selected transport
   peer is copied out and then required exactly for the completing ACK or NAK.
   Keeping that policy separate from the address matters because 0.0.0.0 is
   itself a possible source and must become an exact peer once selected.
   This also binds a relayed exchange to the relay endpoint that supplied the
   offer rather than confusing option 54 with the UDP sender. */
static bool dhcp_peer_matches(
    const socket_address_internet address_to peer, p32 peer_size,
    const socket_address_internet address_to expected, bool any_host)
{
        return peer && expected && peer_size == sizeof(*peer) &&
               peer->family == AF_INET && expected->family == AF_INET &&
               peer->port == expected->port &&
               (any_host || peer->host == expected->host);
}

static bool dhcp_receive(bipolar handle, p8 address_to packet, positive room,
                         p32 transaction, p8 address_to hardware,
                         dhcp_lease address_to lease, p8 address_to kind,
                         const socket_address_internet address_to expected_peer,
                         bool any_peer_host,
                         socket_address_internet address_to accepted_peer,
                         const network_deadline address_to deadline)
{
        bipolar got;

        for (;;)
        {
                got = network_wait_readable_until(handle, deadline);

                if (got <= 0)
                        return false;

                socket_address_internet peer;
                p32 peer_size = sizeof peer;

                memory_fill(address_of peer, 0, sizeof peer);
                got = socket_receive((b32)handle, packet, room, MSG_TRUNC,
                                     address_of peer, address_of peer_size);

                if (got == NETWORK_INTERRUPTED)
                        continue;
                if (got < 0)
                        return false;
                if (!got)
                        continue;

                if ((positive)got <= room &&
                    dhcp_peer_matches(address_of peer, peer_size,
                                      expected_peer, any_peer_host) &&
                    dhcp_read(packet, (positive)got, transaction, hardware,
                              lease, kind) >= 0)
                {
                        if (accepted_peer)
                                *accepted_peer = peer;
                        return true;
                }
        }
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
        kernel's explicit early-boot stream answers until the pool is ready.

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
        if (!dhcp_transaction_early(address_of transaction))
                return DHCP_NO_RANDOM;
        handle = dhcp_open(device, HOST_ANY, true);

        if (handle < 0)
                return DHCP_NO_SOCKET;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT),
            .host = network_order_32(HOST_BROADCAST)};
        socket_address_internet any_server = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT)};
        socket_address_internet selected_peer;

        for (attempt = 0; attempt < 20; attempt++)
        {
                p8 kind = 0;
                network_deadline deadline;

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
                length = dhcp_build(packet, sizeof packet, DHCP_DISCOVER,
                                    transaction, hardware, 0, 0, 0, true);

                //      A failed send is most likely no route yet. An
                //      unconnected UDP socket routes every send afresh, so
                //      carrier can appear underneath it; the attempt still
                //      waits out its interval rather than spending the whole
                //      schedule in one instant.
                (void)socket_send((b32)handle, packet, length, 0,
                                  address_of where, sizeof where);

                if (!network_deadline_begin(
                        address_of deadline, wait / 4,
                        (wait % 4) * 250000000))
                        continue;

                while (dhcp_receive(handle, packet, sizeof packet, transaction,
                                    hardware, lease, address_of kind,
                                    address_of any_server,
                                    true,
                                    address_of selected_peer,
                                    address_of deadline))
                {
                        if (kind != DHCP_OFFER || !dhcp_lease_usable(lease))
                                continue;

                        //      Take the offer, naming the server so that any
                        //      other server that offered knows it lost.
                        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST,
                                            transaction, hardware, lease->address,
                                            lease->server, 0, true);

                        if (socket_send(
                                (b32)handle, packet, length, 0,
                                address_of where, sizeof where) < 0)
                                /* The REQUEST never left.  Start the next
                                   discovery attempt immediately instead of
                                   spending its whole reply budget waiting for
                                   an answer that cannot exist. */
                                break;

                        dhcp_lease answer = {0};

                        if (!network_deadline_begin(
                                address_of deadline, wait / 4,
                                (wait % 4) * 250000000))
                                break;

                        while (dhcp_receive(
                                   handle, packet, sizeof packet,
                                   transaction, hardware, address_of answer,
                                   address_of kind, address_of selected_peer,
                                   false, null, address_of deadline))
                        {
                                if (dhcp_acquisition_answer_matches(
                                        kind, address_of answer, lease))
                                {
                                        if (kind == DHCP_ACK)
                                        {
                                            if (!dhcp_lease_acknowledge(
                                                    lease,
                                                    address_of answer))
                                                    continue;
                                        }
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

        A timeout here is not fatal and does not discard the address. The
        caller retries within the current state, switches from unicast renewal
        to broadcast rebinding at T2, and starts discovery only after a NAK or
        the lease's actual expiry.
*/
static bipolar dhcp_reacquire(string_address device, p8 address_to hardware,
                              dhcp_lease address_to lease, bool rebinding,
                              positive wait)
{
        p8 packet[1024];
        p32 transaction;
        dhcp_lease fresh;
        bipolar handle;
        bipolar status = DHCP_NO_OFFER;
        positive length;
        network_deadline deadline;
        p8 kind = 0;

        if (!dhcp_lease_usable(lease) || !wait)
                return DHCP_NO_OFFER;

        if (!dhcp_transaction_early(address_of transaction))
                return DHCP_NO_RANDOM;
        handle = dhcp_open(device, lease->address, rebinding);

        if (handle < 0)
                return DHCP_NO_SOCKET;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT),
            .host = network_order_32(rebinding ? HOST_BROADCAST
                                               : lease->server)};
        socket_address_internet expected = {
            .family = AF_INET, .port = network_order_16(DHCP_SERVER_PORT),
            .host = network_order_32(lease->server)};

        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST, transaction,
                            hardware, 0, 0, lease->address, rebinding);

        if (socket_send((b32)handle, packet, length, 0, address_of where,
                        sizeof where) < 0)
        {
                status = DHCP_NO_SOCKET;
                goto done;
        }

        if (!network_deadline_begin(address_of deadline, wait, 0))
                goto done;

        while (true)
        {
                memory_fill(address_of fresh, 0, sizeof fresh);

                if (!dhcp_receive(handle, packet, sizeof packet, transaction,
                                  hardware, address_of fresh, address_of kind,
                                  address_of expected, rebinding, null,
                                  address_of deadline))
                        break;

                if (kind == DHCP_ACK &&
                    dhcp_reacquisition_answer_matches(
                        kind, address_of fresh, lease, rebinding))
                {
                        //      Keep what the renewal said, including the new
                        //      lease time, but do not lose what it left out:
                        //      an ACK need not repeat every option.
                        if (!dhcp_lease_acknowledge(
                                lease, address_of fresh))
                                continue;

                        status = DHCP_OK;
                        break;
                }

                if (kind == DHCP_NAK &&
                    dhcp_reacquisition_answer_matches(
                        kind, address_of fresh, lease, rebinding))
                {
                        status = DHCP_REFUSED;
                        break;
                }
        }

done:
        socket_close((b32)handle);
        return status;
}

static bipolar dhcp_renew(string_address device, p8 address_to hardware,
                          dhcp_lease address_to lease, positive wait)
{
        return dhcp_reacquire(device, hardware, lease, false, wait);
}

static bipolar dhcp_rebind(string_address device, p8 address_to hardware,
                           dhcp_lease address_to lease, positive wait)
{
        return dhcp_reacquire(device, hardware, lease, true, wait);
}

#endif // STANDARD_MODERN_C_NET_DHCP
