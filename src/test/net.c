#include "../compiler_memory.c"
#include "../net/netlink.c"
/*
        The netlink wire, and the messages built on it.

        Two halves, the way src/test/socket.c has two.

        The first is hermetic: a message is built and its bytes are compared
        against what they must be, byte for byte. That half needs no network,
        no privileges and no kernel co-operation, so it is the half that runs
        identically on all three architectures and catches the mistakes this
        protocol invites -- a length rounded up before it is written rather
        than after, an attribute whose payload is padded but whose length says
        so, a field reversed into the wire's byte order when netlink wants the
        machine's.

        The second half talks to the running kernel. Reading is always
        allowed, so the link dump and its answers are checked everywhere.
        Changing an interface is not, so those are attempted and their refusal
        accepted: under `unshare -rn` they run for real, and the lane says
        which of the two it did rather than quietly testing less than it
        looks.
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

static fn arithmetic(void)
{
        //      The rounding every length in this protocol goes through.
        check("align 0", netlink_align(0) == 0);
        check("align 1", netlink_align(1) == 4);
        check("align 3", netlink_align(3) == 4);
        check("align 4", netlink_align(4) == 4);
        check("align 5", netlink_align(5) == 8);
        check("align 16", netlink_align(16) == 16);
        check("align 17", netlink_align(17) == 20);

        //      Sizes the kernel checks against, measured on the build machine.
        check("header is 16", sizeof(netlink_header) == 16);
        check("link body is 16", sizeof(netlink_link) == 16);
        check("address body is 8", sizeof(netlink_address) == 8);
        check("route body is 12", sizeof(netlink_route) == 12);
        check("attribute is 4", sizeof(netlink_attribute) == 4);
}

static fn building(void)
{
        netlink_buffer request = {0};
        netlink_header address_to header;
        netlink_address address_to body;
        p8 address_to bytes;
        p32 host = 0x0a000203;
        p32 wire = network_order_32(host);

        check("begin an address request",
              netlink_begin(address_of request, RTM_NEWADDR,
                            NLM_REQUEST | NLM_ACK | NLM_CREATE | NLM_REPLACE,
                            0x11223344, sizeof(netlink_address)));

        body = (netlink_address address_to)netlink_body(address_of request);
        body->family = AF_INET;
        body->prefix = 24;
        body->scope = RT_SCOPE_UNIVERSE;
        body->index = 2;

        check("one attribute",
              netlink_attribute_add(address_of request, IFA_LOCAL, address_of wire, 4));
        check("and another",
              netlink_attribute_add(address_of request, IFA_ADDRESS, address_of wire, 4));

        header = (netlink_header address_to)request.bytes;
        bytes = request.bytes;

        //      16 header + 8 body + two attributes of 8 each.
        check("length counts every attribute", header->length == 40);
        check("cursor agrees with the length", request.used == 40);
        check("type survived", header->type == RTM_NEWADDR);
        check("sequence survived", header->sequence == 0x11223344);

        //      netlink's own fields are the machine's byte order, so the
        //      length reads as 40 in the first byte on a little endian
        //      machine and would read as 0 if something had reversed it.
        check("length is not byte reversed", bytes[0] == 40 && bytes[1] == 0);

        check("family byte", bytes[16] == AF_INET);
        check("prefix byte", bytes[17] == 24);
        check("index word", address_to((p32 address_to)(bytes + 20)) == 2);

        //      First attribute: length 8, type IFA_LOCAL, then the address in
        //      wire order -- 10.0.2.3 is 0a 00 02 03 in that order on every
        //      machine, which is the whole point of it being wire order.
        check("attribute length", address_to((p16 address_to)(bytes + 24)) == 8);
        check("attribute type", address_to((p16 address_to)(bytes + 26)) == IFA_LOCAL);
        check("address is big endian", bytes[28] == 0x0a && bytes[29] == 0x00 &&
                                       bytes[30] == 0x02 && bytes[31] == 0x03);
        check("second attribute type",
              address_to((p16 address_to)(bytes + 34)) == IFA_ADDRESS);

        netlink_forget(address_of request);
        check("forgetting gives the room back", request.bytes == null);
}

//      An attribute whose payload is not a multiple of four: the length is the
//      true one, the cursor moves by the padded one, and the pad is zero.
static fn padding(void)
{
        netlink_buffer request = {0};
        p8 name[4] = {'e', 't', 'h', '0'};

        netlink_begin(address_of request, RTM_NEWLINK, NLM_REQUEST, 7,
                      sizeof(netlink_link));
        netlink_attribute_add(address_of request, IFLA_IFNAME, name, 5);

        check("odd attribute keeps its true length",
              address_to((p16 address_to)(request.bytes + 32)) == 9);
        check("cursor moved by the padded length", request.used == 32 + 12);
        check("the pad is zeroed", request.bytes[32 + 9] == 0 &&
                                   request.bytes[32 + 10] == 0 &&
                                   request.bytes[32 + 11] == 0);

        netlink_forget(address_of request);
}

static fn talking(void)
{
        netlink_search search;
        bipolar handle = netlink_open();
        bipolar status;

        check("a netlink socket opens", handle >= 0);

        if (handle < 0)
                return;

        //      Loopback is on every machine and is always index 1 on Linux.
        memory_fill(address_of search, 0, sizeof search);
        search.wanted = (string_address) "lo";
        status = netlink_link_find((b32)handle, address_of search);

        check("the link dump finds lo", status == 0);
        check("lo is index 1", search.index == 1);
        check("lo says it is loopback", (search.flags & IFF_LOOPBACK) != 0);

        //      A name nobody has: the dump completes and finds nothing,
        //      which is a different answer from the dump failing.
        memory_fill(address_of search, 0, sizeof search);
        search.wanted = (string_address) "nosuchlink0";
        status = netlink_link_find((b32)handle, address_of search);
        check("an unknown name is refused, not found", status == -19);

        //      Whatever is not loopback, if anything is.
        memory_fill(address_of search, 0, sizeof search);
        search.skip_loopback = true;
        status = netlink_link_find((b32)handle, address_of search);
        check("discovery either finds a link or says there is none",
              status == 0 || status == -19);

        if (status == 0)
        {
                check("a discovered link has an index", search.index != 0);
                check("a discovered link is not loopback",
                      (search.flags & IFF_LOOPBACK) == 0);
                string_format(log, "  first non-loopback link: %s (index %p)\n",
                              search.name, (positive)search.index);
        }
        else
        {
                string_format(log, "  no non-loopback link on this machine\n");
        }

        /*
                Changing things needs privilege this may not have. Raising
                loopback is the safest possible mutation: it is already up on
                any running machine, so success changes nothing, and under
                unshare -rn it is genuinely down and genuinely comes up.
        */
        status = netlink_link_up((b32)handle, 1);
        check("bringing loopback up either works or is refused",
              status == 0 || status == -1 || status == -13);

        if (status != 0)
        {
                string_format(log, "  interfaces are read only here (%p)\n",
                              (positive)(-status));
                socket_close((b32)handle);
                return;
        }

        string_format(log, "  interfaces are writable here\n");

        /*
                The whole point, end to end.

                An address is added and then proved to be there by something
                other than the call that added it: an ordinary AF_INET socket
                is bound to it, which only succeeds if the kernel genuinely
                believes the address is on an interface. An address nobody
                added is bound to as well, and has to fail -- otherwise the
                first bind proves nothing except that bind returns zero.
        */
        {
                socket_address_internet where;
                bipolar probe;
                p32 mine = 0x0a090807;    // 10.9.8.7
                //      Not 10.9.8.6. Adding an address to loopback makes the
                //      kernel install a local route for the whole prefix --
                //      "local 10.9.8.0/24 dev lo" -- and not merely the /32,
                //      so every address in the subnet binds whether it was
                //      added or not. This one is TEST-NET-1, outside anything
                //      configured, which is what makes the refusal mean
                //      something.
                p32 absent = 0xc0000201;  // 192.0.2.1

                check("an address is added",
                      netlink_address_add((b32)handle, 1, mine, 24) == 0);

                //      Adding it twice is the same as adding it once, which
                //      is what REPLACE is for: a retried boot must not fail.
                check("adding it again is not an error",
                      netlink_address_add((b32)handle, 1, mine, 24) == 0);

                probe = socket_new(AF_INET, SOCK_DGRAM, 0);
                check("a probe socket opens", probe >= 0);

                memory_fill(address_of where, 0, sizeof where);
                where.family = AF_INET;
                where.host = network_order_32(mine);
                check("the address is really on the interface",
                      socket_bind((b32)probe, address_of where, sizeof where) == 0);
                socket_close((b32)probe);

                probe = socket_new(AF_INET, SOCK_DGRAM, 0);
                memory_fill(address_of where, 0, sizeof where);
                where.family = AF_INET;
                where.host = network_order_32(absent);
                check("an address nobody added is not there",
                      socket_bind((b32)probe, address_of where, sizeof where) < 0);
                socket_close((b32)probe);

                //      A route through a gateway the address above covers.
                check("a route is added",
                      netlink_route_add((b32)handle, 0, 0, 0x0a090801, 1) == 0);

                //      And one through a gateway no configured address can
                //      reach, which the kernel refuses -- "Nexthop has invalid
                //      gateway" -- rather than accepting and never using it.
                //      Not a 127 address: loopback is up, so those are
                //      perfectly reachable and would be accepted.
                check("an unreachable gateway is refused",
                      netlink_route_add((b32)handle, 0, 0, 0xc0000201, 1) != 0);
        }

        socket_close((b32)handle);
}

b32 main(void)
{
        arithmetic();
        building();
        padding();
        talking();

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
