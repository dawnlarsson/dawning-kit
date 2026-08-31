/*
        Experimental C standard library

        rtnetlink: a link brought up, an address given, a route added

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_NETLINK
#define STANDARD_MODERN_C_NET_NETLINK

/*
        This is ordinary C on purpose.

        library.c and everything it includes holds declarations and assembly
        and nothing else, which is checked. What follows is a cursor walking a
        buffer appending length-prefixed attributes -- function bodies, and
        with nothing in them a machine would do differently from any other
        machine. Putting it here rather than in the assembly graph is what
        lets it be written once instead of three times, and what lets the test
        include it directly and run the same bytes on all three targets.

        It depends on library.c alone. No shell anything: the freestanding
        test builds this file without a shell in sight.

        Everything below is measured, not remembered. The offsets came out of
        offsetof on the build machine and the constants out of the headers
        beside them, and the test pins the ones a wrong guess would silently
        survive.

        The one thing worth saying twice: netlink's own fields are in the
        machine's byte order, not the wire's. Only what travels inside an
        attribute as an address is big endian. Reversing an nlmsg_len is the
        mistake this protocol invites, and it produces a message the kernel
        answers with EINVAL and no explanation.
*/

#define NETLINK_ALIGN 4
#define NETLINK_HEADER 16

#define NLM_REQUEST 0x0001
#define NLM_MULTI 0x0002
#define NLM_ACK 0x0004
#define NLM_DUMP 0x0300
#define NLM_DUMP_INTERRUPTED 0x0010
#define NLM_REPLACE 0x0100
#define NLM_EXCLUSIVE 0x0200
#define NLM_CREATE 0x0400
#define NLM_CAPPED 0x0100

#define NLMSG_IS_NOOP 1
#define NLMSG_IS_ERROR 2
#define NLMSG_IS_DONE 3

#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26

#define IFLA_ADDRESS 1
#define IFLA_IFNAME 3
#define IFLA_MTU 4

#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3

#define RTA_DST 1
#define RTA_OIF 4
#define RTA_GATEWAY 5

#define RT_TABLE_MAIN 254
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK 253
#define RTPROT_BOOT 3
#define RTN_UNICAST 1

#define IFF_UP 1
#define IFF_BROADCAST 2
#define IFF_LOOPBACK 8
#define IFF_RUNNING 64

#define IFNAME_SIZE 16

//      Every field here is the machine's own byte order.
typedef struct
{
        p32 length;
        p16 type;
        p16 flags;
        p32 sequence;
        p32 port;
} netlink_header;

//      ifinfomsg. The byte after the family is padding the kernel does read
//      back as zero, so it is named rather than left to whatever was there.
typedef struct
{
        p8 family;
        p8 padding;
        p16 kind;
        p32 index;
        p32 flags;
        p32 change;
} netlink_link;

//      ifaddrmsg
typedef struct
{
        p8 family;
        p8 prefix;
        p8 flags;
        p8 scope;
        p32 index;
} netlink_address;

//      rtmsg
typedef struct
{
        p8 family;
        p8 destination_bits;
        p8 source_bits;
        p8 tos;
        p8 table;
        p8 protocol;
        p8 scope;
        p8 kind;
        p32 flags;
} netlink_route;

//      rtattr
typedef struct
{
        p16 length;
        p16 type;
} netlink_attribute;

#define netlink_align(value) (((value) + 3) & ~(positive)3)

/*
        A message being built, and the room it is being built in.

        Nothing here has a fixed size either. A request is small, but a dump
        of the links on a machine with a hundred interfaces is not, and the
        size of the answer is not known until the kernel has said it: a read
        asking for MSG_TRUNC is told the true length even when it did not fit,
        which is what makes growing to it and reading again possible instead
        of guessing at a ceiling and calling the remainder someone else's
        problem.
*/
typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
        bool failed;
} netlink_buffer;

static bool net_room(netlink_buffer address_to buffer, positive want)
{
        if (!memory_reserve((address_any address_to)address_of buffer->bytes,
                            address_of buffer->room, buffer->used, want, 1, 4096))
        {
                buffer->failed = true;
                return false;
        }

        return true;
}

static fn netlink_forget(netlink_buffer address_to buffer)
{
        memory_release((address_any address_to)address_of buffer->bytes,
                       address_of buffer->room, address_of buffer->used, 1);
        buffer->failed = false;
}

//      A fresh request: the header, then the family-specific body after it.
static bool netlink_begin(netlink_buffer address_to buffer, p16 type, p16 flags,
                          p32 sequence, positive body)
{
        netlink_header address_to header;

        buffer->used = 0;
        buffer->failed = false;

        if (!net_room(buffer, NETLINK_HEADER + netlink_align(body) + 64))
        {
                netlink_forget(buffer);
                return false;
        }

        memory_fill(buffer->bytes, 0, NETLINK_HEADER + netlink_align(body));

        header = (netlink_header address_to)buffer->bytes;
        header->length = (p32)(NETLINK_HEADER + body);
        header->type = type;
        header->flags = flags;
        header->sequence = sequence;
        header->port = 0;

        buffer->used = NETLINK_HEADER + netlink_align(body);

        return true;
}

//      The body, for the caller to fill in by name rather than by offset.
static address_any netlink_body(netlink_buffer address_to buffer)
{
        return buffer->bytes + NETLINK_HEADER;
}

/*
        One attribute appended.

        The length written into the attribute is the true length, header
        included and padding excluded; what the cursor moves by is that length
        rounded up to four. Writing the rounded length into the attribute is
        the error that makes a message the kernel accepts and misreads.
*/
static bool netlink_attribute_add(netlink_buffer address_to buffer, p16 type,
                                  address_any data, positive size)
{
        netlink_attribute address_to attribute;
        positive length = sizeof(netlink_attribute) + size;
        positive padded = netlink_align(length);
        netlink_header address_to header;

        if (buffer->failed)
                return false;

        if (!net_room(buffer, buffer->used + padded))
                return false;

        attribute = (netlink_attribute address_to)(buffer->bytes + buffer->used);
        attribute->length = (p16)length;
        attribute->type = type;

        if (size)
                memory_copy(buffer->bytes + buffer->used + sizeof(netlink_attribute),
                            data, size);

        //      The pad is sent, so it is zeroed rather than left as whatever
        //      the mapping held.
        if (padded > length)
                memory_fill(buffer->bytes + buffer->used + length, 0, padded - length);

        buffer->used += padded;

        header = (netlink_header address_to)buffer->bytes;
        header->length += (p32)padded;

        return true;
}

/*
        A netlink socket, bound.

        The bind is what gives this socket a port so replies can find it, and
        a zero port asks the kernel to pick one -- which matters, because a
        shell may have forked and two processes claiming the same port is the
        one netlink error that looks like a hang rather than a failure.

        EXT_ACK is asked for and not required. It costs one setsockopt and
        turns "-EINVAL" into a sentence naming the attribute that was wrong;
        a kernel too old to know the option says so and everything else works
        exactly as before.
*/
#define RTNLGRP_LINK_MASK 1

static bipolar netlink_open_groups(p32 groups)
{
        bipolar handle = socket_new(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
        b32 want = 1;

        if (handle < 0)
                return handle;

        socket_address_netlink self = {.family = AF_NETLINK, .groups = groups};

        if (socket_bind((b32)handle, address_of self, sizeof self) < 0)
        {
                socket_close((b32)handle);
                return -1;
        }

        socket_option_set((b32)handle, SOL_NETLINK, NETLINK_EXT_ACK,
                          address_of want, sizeof want);

        return handle;
}

static bipolar netlink_open(void)
{
        return netlink_open_groups(0);
}

/*
        One datagram, however big it turns out to be.

        The first read only looks: MSG_PEEK leaves the datagram where it is
        and MSG_TRUNC makes the kernel answer with its true length rather than
        the length that fitted. So the room is taken knowing the size, and the
        second read is the one that consumes it. Without MSG_TRUNC a message
        larger than the buffer arrives silently shortened, and a dump that
        loses its tail looks exactly like a dump that ended.
*/
static bipolar netlink_receive(b32 handle, netlink_buffer address_to buffer)
{
        bipolar size = socket_receive(handle, buffer->bytes ? buffer->bytes : (address_any)&size,
                                      buffer->bytes ? buffer->room : 0,
                                      MSG_PEEK | MSG_TRUNC, 0, 0);
        bipolar got;

        if (size < 0)
                return size;

        if (!net_room(buffer, (positive)size + 64))
                return -1;

        got = socket_receive(handle, buffer->bytes, buffer->room, 0, 0, 0);

        if (got >= 0)
                buffer->used = (positive)got;

        return got;
}

typedef bool (address_to netlink_visitor)(netlink_header address_to header,
                                          address_any context);

static bipolar netlink_walk(b32 handle, netlink_buffer address_to request,
                            p32 sequence, netlink_buffer address_to reply,
                            netlink_visitor visit, address_any context);

/*
        The tail every acknowledged request shares: sent, answered, and both
        buffers given back whatever the answer was.
*/
static bipolar netlink_transact(b32 handle, netlink_buffer address_to request,
                                p32 sequence, netlink_buffer address_to reply,
                                netlink_visitor visit, address_any context)
{
        bipolar status = netlink_walk(handle, request, sequence, reply,
                                      visit, context);

        netlink_forget(request);
        netlink_forget(reply);

        return status;
}

/*
        A dump walked, one message at a time.

        A dump is many messages, possibly spread over several datagrams, and
        it ends with an NLMSG_DONE rather than by running out. Every message
        carries NLM_MULTI until that one.

        DUMP_INTR is the kernel saying the table changed underneath the walk,
        so what was collected may have missed an entry or counted one twice.
        Early boot is exactly when interfaces appear, so it is checked, and
        the answer to it is to start again rather than to hand back a list
        that is quietly wrong.

        A visitor that has seen enough stops being called, but the walk does
        not stop reading. A dump the kernel considers unfinished stays in
        progress on the socket, and the next dump asked for on it is refused
        with EBUSY -- which arrives long after the abandoned one, attached to
        an innocent request, and reads as though the second question were the
        problem. So the remaining messages are drawn and dropped, and the
        socket is handed back clean.
*/
static bipolar netlink_walk(b32 handle, netlink_buffer address_to request,
                            p32 sequence, netlink_buffer address_to reply,
                            netlink_visitor visit, address_any context)
{
        netlink_header address_to header;
        bool enough = false;
        bipolar sent;
        positive at;

        if (request->failed)
                return -1;

        sent = socket_send(handle, request->bytes, request->used, 0, 0, 0);

        if (sent < 0)
                return sent;

        for (;;)
        {
                bipolar got = netlink_receive(handle, reply);

                if (got < 0)
                        return got;

                at = 0;

                while (at + NETLINK_HEADER <= reply->used)
                {
                        header = (netlink_header address_to)(reply->bytes + at);

                        if (header->length < NETLINK_HEADER ||
                            at + header->length > reply->used)
                                return -1;

                        if (header->sequence != sequence)
                        {
                                at += netlink_align(header->length);
                                continue;
                        }

                        if (header->flags & NLM_DUMP_INTERRUPTED)
                                return -1;

                        if (header->type == NLMSG_IS_DONE)
                                return 0;

                        if (header->type == NLMSG_IS_ERROR)
                                return address_to(
                                    (b32 address_to)(reply->bytes + at + NETLINK_HEADER));

                        if (!enough && header->type != NLMSG_IS_NOOP && visit &&
                            !visit(header, context))
                                enough = true;

                        at += netlink_align(header->length);
                }
        }
}

/*
        The attributes of one message, found by type.

        The body length differs per message family, so the caller says where
        the attributes begin. A length shorter than its own header would walk
        backwards forever, which is the shape a corrupt or hostile message
        takes, so it ends the walk.
*/
static address_any netlink_find(netlink_header address_to header, positive body,
                                p16 type, positive address_to size)
{
        p8 address_to bytes = (p8 address_to)header;
        positive at = NETLINK_HEADER + netlink_align(body);
        netlink_attribute address_to attribute;

        while (at + sizeof(netlink_attribute) <= header->length)
        {
                attribute = (netlink_attribute address_to)(bytes + at);

                if (attribute->length < sizeof(netlink_attribute) ||
                    at + attribute->length > header->length)
                        return null;

                if (attribute->type == type)
                {
                        if (size)
                                address_to size = attribute->length -
                                                  sizeof(netlink_attribute);

                        return bytes + at + sizeof(netlink_attribute);
                }

                at += netlink_align(attribute->length);
        }

        return null;
}

/*
        What the four things actually are, on the wire.

        Each is a request built and acknowledged, and every one of them has a
        detail that is only obvious after the kernel has refused it once:

        A link's flags are set with a change mask, and a change mask of zero
        does not mean "change nothing". The kernel reads it as ~0 -- "this is
        the whole flag word now" -- so a request meaning to raise IFF_UP and
        leaving change at zero silently clears everything else the interface
        had. It is a required argument here for that reason, not an optional
        one with a convenient default.

        An IPv4 address needs IFA_LOCAL. Sending only IFA_ADDRESS, which is
        the attribute whose name suggests it holds the address, fails with
        EINVAL and the ext_ack sentence "ipv4: Local address is not supplied".
        Both are sent, which is what iproute2 does, and which is also right
        for a point-to-point link where the two genuinely differ.

        A route needs NLM_CREATE. Without it the kernel is being asked to
        change a route that is not there yet and answers ENOENT.
*/

static p32 netlink_sequence_next = 1;

typedef struct
{
        string_address wanted;
        p32 index;
        p32 flags;
        bool found;
        bool skip_loopback;
        bool has_hardware;
        p8 name[IFNAME_SIZE];
        p8 hardware[6];
} netlink_search;

static bool netlink_link_seen(netlink_header address_to header, address_any context)
{
        netlink_search address_to search = (netlink_search address_to)context;
        netlink_link address_to link = (netlink_link address_to)((p8 address_to)header +
                                                                 NETLINK_HEADER);
        positive size = 0;
        string_address name = (string_address)netlink_find(header, sizeof(netlink_link),
                                                           IFLA_IFNAME, address_of size);

        if (!name)
                return true;

        if (search->skip_loopback)
        {
                if (link->flags & IFF_LOOPBACK)
                        return true;

                /*
                        Not the first one found -- the best one.

                        A machine with wired and wireless has both, and the
                        one worth configuring is the one with something
                        plugged into it. IFF_RUNNING is the kernel saying the
                        link has carrier, so a candidate that has it beats one
                        that does not, whatever order the dump arrived in, and
                        the walk goes all the way to the end rather than
                        stopping at whatever came first.
                */
                if (search->found)
                {
                        bool had = (search->flags & IFF_RUNNING) != 0;
                        bool has = (link->flags & IFF_RUNNING) != 0;

                        if (had || !has)
                                return true;
                }
        }
        else if (search->found || !string_equals(name, search->wanted))
        {
                return true;
        }

        search->index = link->index;
        search->flags = link->flags;
        search->found = true;
        string_copy_max_end(search->name, name, IFNAME_SIZE - 1);

        //      The hardware address, which DHCP has to put in the packet and
        //      which is the only way a reply finds its way back before there
        //      is an address to send it to.
        {
                positive width = 0;
                p8 address_to found_hardware = (p8 address_to)netlink_find(
                    header, sizeof(netlink_link), IFLA_ADDRESS, address_of width);

                if (found_hardware && width == 6)
                {
                        memory_copy(search->hardware, found_hardware, 6);
                        search->has_hardware = true;
                }
        }

        return search->skip_loopback;
}

/*
        The index of a link, by name, or of the first that is not loopback.

        A single-shot RTM_GETLINK can be asked about a name directly, but that
        is no use for discovery: the whole question at boot is what the
        interface is called. So this is always the dump, and the name is
        matched while walking it. /sys would answer the same question, but the
        boot image mounts only devpts, so there is no /sys/class/net to read.
*/
static bipolar netlink_link_find(b32 handle, netlink_search address_to search)
{
        netlink_buffer request = {0};
        netlink_buffer reply = {0};
        netlink_link address_to body;
        p32 sequence = netlink_sequence_next++;
        bipolar status;

        search->found = false;

        if (!netlink_begin(address_of request, RTM_GETLINK, NLM_REQUEST | NLM_DUMP,
                           sequence, sizeof(netlink_link)))
                return -1;

        body = (netlink_link address_to)netlink_body(address_of request);
        body->family = AF_UNSPEC;

        status = netlink_transact(handle, address_of request, sequence,
                                  address_of reply, netlink_link_seen, search);

        if (status < 0)
                return status;

        return search->found ? 0 : -19;
}

//      IFF_UP raised, and every other flag left exactly as it was found.
static bipolar netlink_link_up(b32 handle, p32 index)
{
        netlink_buffer request = {0};
        netlink_buffer reply = {0};
        netlink_link address_to body;
        p32 sequence = netlink_sequence_next++;

        if (!netlink_begin(address_of request, RTM_NEWLINK,
                           NLM_REQUEST | NLM_ACK, sequence, sizeof(netlink_link)))
                return -1;

        body = (netlink_link address_to)netlink_body(address_of request);
        body->family = AF_UNSPEC;
        body->index = index;
        body->flags = IFF_UP;
        body->change = IFF_UP;

        return netlink_transact(handle, address_of request, sequence,
                                address_of reply, null, null);
}

/*
        An address given to a link.

        REPLACE rather than EXCLUSIVE, so running the same command twice is
        the same as running it once. A boot that is retried should not fail
        because the first attempt succeeded.
*/
static bipolar netlink_address_add(b32 handle, p32 index, p32 host, p8 prefix)
{
        netlink_buffer request = {0};
        netlink_buffer reply = {0};
        netlink_address address_to body;
        p32 sequence = netlink_sequence_next++;
        p32 wire = network_order_32(host);

        if (!netlink_begin(address_of request, RTM_NEWADDR,
                           NLM_REQUEST | NLM_ACK | NLM_CREATE | NLM_REPLACE,
                           sequence, sizeof(netlink_address)))
                return -1;

        body = (netlink_address address_to)netlink_body(address_of request);
        body->family = AF_INET;
        body->prefix = prefix;
        body->scope = RT_SCOPE_UNIVERSE;
        body->index = index;

        netlink_attribute_add(address_of request, IFA_LOCAL, address_of wire, 4);
        netlink_attribute_add(address_of request, IFA_ADDRESS, address_of wire, 4);

        return netlink_transact(handle, address_of request, sequence,
                                address_of reply, null, null);
}

/*
        A route, with a destination of no bits at all being the default one.

        The gateway has to be reachable already, which for a default route
        means the address added above has to cover it. The kernel says ENETUNREACH
        when it does not, which reads as a network problem and is really an
        ordering one.
*/
static bipolar netlink_route_add(b32 handle, p32 destination, p8 bits, p32 gateway,
                                 p32 index)
{
        netlink_buffer request = {0};
        netlink_buffer reply = {0};
        netlink_route address_to body;
        p32 sequence = netlink_sequence_next++;
        p32 wire_gateway = network_order_32(gateway);
        p32 wire_destination = network_order_32(destination);

        //      REPLACE alongside CREATE, for the same reason the address
        //      add has it: adding the route a second time is what happens
        //      when a link comes back, and it should be the same as having
        //      added it once rather than EEXIST.
        if (!netlink_begin(address_of request, RTM_NEWROUTE,
                           NLM_REQUEST | NLM_ACK | NLM_CREATE | NLM_REPLACE,
                           sequence, sizeof(netlink_route)))
                return -1;

        body = (netlink_route address_to)netlink_body(address_of request);
        body->family = AF_INET;
        body->destination_bits = bits;
        body->table = RT_TABLE_MAIN;
        body->protocol = RTPROT_BOOT;
        body->scope = gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
        body->kind = RTN_UNICAST;

        if (bits)
                netlink_attribute_add(address_of request, RTA_DST,
                                      address_of wire_destination, 4);

        if (gateway)
                netlink_attribute_add(address_of request, RTA_GATEWAY,
                                      address_of wire_gateway, 4);

        if (index)
                netlink_attribute_add(address_of request, RTA_OIF,
                                      address_of index, 4);

        return netlink_transact(handle, address_of request, sequence,
                                address_of reply, null, null);
}

/*
        Everything of one kind, walked.

        Links, addresses and routes all begin their body with a family byte,
        so one request shape asks for all three. The body size differs and the
        caller says which, because it is also what tells netlink_find where
        the attributes start.
*/
static bipolar netlink_dump(b32 handle, p16 type, positive body, p8 family,
                            netlink_visitor visit, address_any context)
{
        netlink_buffer request = {0};
        netlink_buffer reply = {0};
        p32 sequence = netlink_sequence_next++;
        if (!netlink_begin(address_of request, type, NLM_REQUEST | NLM_DUMP,
                           sequence, body))
                return -1;

        address_to(p8 address_to) netlink_body(address_of request) = family;

        return netlink_transact(handle, address_of request, sequence,
                                address_of reply, visit, context);
}

#endif // STANDARD_MODERN_C_NET_NETLINK
