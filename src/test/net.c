#include "../compiler_memory.c"
#include "../net/netlink.c"
#include "../net/dns.c"
#include "../net/http.c"
#include "../net/dhcp.c"
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


/*
        The resolver's parsing, without a nameserver.

        Every one of these is a shape a real reply takes and a minimal client
        gets wrong: a name written as labels, a name that points into the
        message rather than repeating itself, and two pointers aimed at each
        other, which is a loop a client that does not count jumps follows
        forever.
*/
static fn resolving(void)
{
        p8 written[64];
        bipolar length;

        length = dns_write_name(written, sizeof written, (string_address) "dawning.dev");
        check("a name is written as labels", length == 13);
        check("the first label is counted", written[0] == 7);
        check("and spelled", memory_compare(written + 1, "dawning", 7) == 0);
        check("the second label is counted", written[8] == 3);
        check("and spelled", memory_compare(written + 9, "dev", 3) == 0);
        check("the name ends in a zero", written[12] == 0);

        length = dns_write_name(written, sizeof written, (string_address) "a.b.c.d.e");
        check("every label is counted", length == 11);

        //      An empty label is what "a..b" and a trailing dot both produce,
        //      and neither is a name.
        check("an empty label is refused",
              dns_write_name(written, sizeof written, (string_address) "a..b") < 0);
        check("no room is refused",
              dns_write_name(written, 4, (string_address) "dawning.dev") < 0);

        {
                //      A message whose second name is a pointer back to the
                //      first: 12 bytes of header, then "dawning.dev", then a
                //      pointer to offset 12.
                p8 message[64];
                bipolar ended;

                memory_fill(message, 0, sizeof message);
                dns_write_name(message + 12, sizeof(message) - 12,
                               (string_address) "dawning.dev");
                message[25] = 0xc0;
                message[26] = 12;

                ended = dns_skip_name(message, 27, 12);
                check("a plain name ends after its zero", ended == 25);

                ended = dns_skip_name(message, 27, 25);
                check("a compressed name ends after its pointer", ended == 27);
        }

        {
                //      Two pointers aimed at each other. A parser that follows
                //      pointers without counting never comes back from this.
                p8 message[8];

                message[0] = 0xc0;
                message[1] = 2;
                message[2] = 0xc0;
                message[3] = 0;

                check("a pointer loop is refused", dns_skip_name(message, 4, 0) < 0);
                check("a forward pointer is refused", dns_skip_name(message, 4, 2) < 0);
        }

        check("a transaction id is not always the same",
              dns_transaction() != dns_transaction() ||
                  dns_transaction() != dns_transaction());
}

//      The URL, the headers and the chunk framing -- all of it pure.
static fn fetching(void)
{
        p8 name[64];
        string_address path;
        p16 port;

        check("a plain url splits",
              http_split((string_address) "http://dawning.dev/index.html", name,
                         sizeof name, address_of port, address_of path) == HTTP_OK);
        check("the host comes out", memory_compare(name, "dawning.dev", 12) == 0);
        check("the port defaults to 80", port == 80);
        check("the path comes out", string_equals(path, (string_address) "/index.html"));

        check("a port is taken",
              http_split((string_address) "http://127.0.0.1:8080/x", name, sizeof name,
                         address_of port, address_of path) == HTTP_OK);
        check("the port is read", port == 8080);
        check("the path after a port", string_equals(path, (string_address) "/x"));

        check("a bare host gets a slash",
              http_split((string_address) "http://dawning.dev", name, sizeof name,
                         address_of port, address_of path) == HTTP_OK);
        check("which is the root", string_equals(path, (string_address) "/"));

        check("https is refused by name",
              http_split((string_address) "https://dawning.dev/", name, sizeof name,
                         address_of port, address_of path) == HTTP_NOT_PLAIN);
        check("an empty host is refused",
              http_split((string_address) "http:///x", name, sizeof name,
                         address_of port, address_of path) == HTTP_BAD_URL);
        check("a port that is not a number is refused",
              http_split((string_address) "http://h:80x/", name, sizeof name,
                         address_of port, address_of path) == HTTP_BAD_URL);

        {
                p8 head[] = "HTTP/1.0 200 OK\r\n"
                            "Content-Type: text/html\r\n"
                            "Content-Length: 42\r\n"
                            "\r\n";
                positive size = sizeof(head) - 1;
                positive length = 0;
                string_address value;

                check("the header block ends at the blank line",
                      http_header_end(head, size) == (bipolar)size);

                value = http_header(head, size, (string_address) "content-length",
                                    address_of length);
                check("a header is found whatever its case", value != null);
                check("and its value read", value && http_number(value, length) == 42);

                check("a header that is not there is not invented",
                      http_header(head, size, (string_address) "location", null) == null);
        }

        {
                //      Two chunks and the terminator, unwrapped in place.
                p8 body[] = "4\r\nabcd\r\n3\r\nefg\r\n0\r\n\r\n";
                bipolar length = http_unchunk(body, sizeof(body) - 1);

                check("chunks unwrap to their contents", length == 7);
                check("and in order", memory_compare(body, "abcdefg", 7) == 0);
        }

        {
                //      A chunk claiming more than arrived, which is what a
                //      truncated response looks like.
                p8 body[] = "9\r\nabc\r\n";

                check("a chunk longer than the body is refused",
                      http_unchunk(body, sizeof(body) - 1) < 0);
        }
}

/*
        A fetch, end to end, over loopback and against no server but our own.

        The child answers one request with a canned response and goes away.
        The parent fetches it the way the command does. Nothing outside this
        machine is involved, so it runs on all three architectures and in any
        order.
*/
static fn fetching_for_real(void)
{
        socket_address_internet where;
        p32 size = sizeof where;
        bipolar listening;
        bipolar child;
        p16 port;
        b32 one = 1;

        listening = socket_new(AF_INET, SOCK_STREAM, 0);
        check("a listening socket opens", listening >= 0);

        if (listening < 0)
                return;

        socket_option_set((b32)listening, SOL_SOCKET, SO_REUSEADDR, address_of one,
                          sizeof one);

        memory_fill(address_of where, 0, sizeof where);
        where.family = AF_INET;
        where.host = network_order_32(HOST_LOOPBACK);
        check("it binds", socket_bind((b32)listening, address_of where, sizeof where) == 0);
        check("it listens", socket_listen((b32)listening, 4) == 0);

        memory_fill(address_of where, 0, sizeof where);
        socket_name((b32)listening, address_of where, address_of size);
        port = network_order_16(where.port);

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                p8 said[256];
                bipolar taken = socket_accept((b32)listening, 0, 0, 0);
                p8 answer[] = "HTTP/1.0 200 OK\r\n"
                              "Content-Length: 11\r\n"
                              "\r\n"
                              "hello there";

                if (taken >= 0)
                {
                        socket_receive((b32)taken, said, sizeof said, 0, 0, 0);
                        system_write_all((positive)taken, answer, sizeof(answer) - 1);
                        socket_shutdown((b32)taken, SHUT_BOTH);
                        socket_close((b32)taken);
                }

                system_call_1(syscall(exit_group), 0);
        }

        {
                http_buffer body = {0};
                b32 code = 0;
                bipolar status = http_get(HOST_LOOPBACK, port,
                                          (string_address) "127.0.0.1",
                                          (string_address) "/", address_of body,
                                          address_of code);
                positive raw = 0;

                check("the fetch succeeds", status == HTTP_OK);
                check("the status line is read", code == 200);
                check("the body is its stated length", body.used == 11);
                check("and is what was sent",
                      body.bytes && memory_compare(body.bytes, "hello there", 11) == 0);

                http_forget(address_of body);
                (void)raw;
        }

        system_wait4_retry((b32)child, null, 0, null);
        socket_close((b32)listening);
}

/*
        The lease, built and read, without a server.

        The socket dance -- broadcast from an address we do not have yet, out
        of an interface the routing table cannot name -- is the part only a
        real network proves, and the boot image does that under qemu. What is
        here is the part that is wrong silently: a field in the wrong byte
        order, an option walked past by the wrong length, a reply for somebody
        else's hardware address believed anyway.
*/
static fn leasing(void)
{
        p8 packet[1024];
        p8 hardware[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        dhcp_lease lease;
        positive length;
        p8 kind = 0;

        length = dhcp_build(packet, sizeof packet, DHCP_DISCOVER, 0xdeadbeef,
                            hardware, 0, 0);

        check("a discover is padded to the length everything accepts", length == 300);
        check("it is a request", packet[0] == 1);
        check("over ethernet", packet[1] == 1);
        check("with six byte addresses", packet[2] == 6);

        //      Big endian, so the most significant byte is first. Reversed,
        //      this would read 0xefbeadde and no reply would ever match.
        check("the transaction is big endian",
              packet[4] == 0xde && packet[5] == 0xad &&
              packet[6] == 0xbe && packet[7] == 0xef);

        check("a broadcast reply is asked for", (packet[10] & 0x80) != 0);
        check("the hardware address is in it",
              memory_compare(packet + 28, hardware, 6) == 0);
        check("the cookie says these options are dhcp's",
              dhcp_read_32(packet + DHCP_HEAD) == DHCP_COOKIE);
        check("the first option is the message type",
              packet[240] == DHCP_OPTION_TYPE && packet[241] == 1 &&
              packet[242] == DHCP_DISCOVER);

        //      A request names the address offered and the server that
        //      offered it, so a second server knows it was not chosen.
        length = dhcp_build(packet, sizeof packet, DHCP_REQUEST, 1, hardware,
                            0x0a00020f, 0x0a000202);
        check("a request carries what was offered",
              packet[243] == DHCP_OPTION_REQUESTED && packet[244] == 4 &&
              dhcp_read_32(packet + 245) == 0x0a00020f);
        check("and who offered it",
              packet[249] == DHCP_OPTION_SERVER && packet[250] == 4 &&
              dhcp_read_32(packet + 251) == 0x0a000202);

        /*
                An offer, in the shape qemu's own server sends one: the
                address in yiaddr and the mask, router, server and lease in
                options, in an order nothing may depend on.
        */
        {
                positive at;

                memory_fill(packet, 0, sizeof packet);
                packet[0] = 2;
                packet[1] = 1;
                packet[2] = 6;
                dhcp_write_32(packet + 4, 0xdeadbeef);
                dhcp_write_32(packet + 16, 0x0a00020f);   // yiaddr 10.0.2.15
                memory_copy(packet + 28, hardware, 6);
                dhcp_write_32(packet + DHCP_HEAD, DHCP_COOKIE);

                at = DHCP_HEAD + 4;
                packet[at++] = DHCP_OPTION_TYPE;  packet[at++] = 1;
                packet[at++] = DHCP_OFFER;
                packet[at++] = DHCP_OPTION_SERVER; packet[at++] = 4;
                dhcp_write_32(packet + at, 0x0a000202); at += 4;
                packet[at++] = DHCP_OPTION_MASK;  packet[at++] = 4;
                dhcp_write_32(packet + at, 0xffffff00); at += 4;
                packet[at++] = DHCP_OPTION_ROUTER; packet[at++] = 4;
                dhcp_write_32(packet + at, 0x0a000202); at += 4;
                packet[at++] = DHCP_OPTION_DNS;   packet[at++] = 4;
                dhcp_write_32(packet + at, 0x0a000203); at += 4;
                packet[at++] = DHCP_OPTION_LEASE; packet[at++] = 4;
                dhcp_write_32(packet + at, 86400); at += 4;
                packet[at++] = DHCP_OPTION_END;

                memory_fill(address_of lease, 0, sizeof lease);
                check("the offer parses",
                      dhcp_read(packet, at, 0xdeadbeef, hardware, address_of lease,
                                address_of kind) == 0);
                check("it is an offer", kind == DHCP_OFFER);
                check("the address is read", lease.address == 0x0a00020f);
                check("the mask is read", lease.mask == 0xffffff00);
                check("which is a /24", dhcp_prefix_of(lease.mask) == 24);
                check("the router is read", lease.router == 0x0a000202);
                check("the nameserver is read", lease.nameserver == 0x0a000203);
                check("the server is read", lease.server == 0x0a000202);
                check("the lease time is read", lease.seconds == 86400);

                //      Somebody else's transaction, and somebody else's
                //      hardware. Both are on the same broadcast domain as us.
                check("another transaction is not ours",
                      dhcp_read(packet, at, 0x11111111, hardware, address_of lease,
                                address_of kind) < 0);

                hardware[5] = 0x57;
                check("another machine's reply is not ours",
                      dhcp_read(packet, at, 0xdeadbeef, hardware, address_of lease,
                                address_of kind) < 0);
                hardware[5] = 0x56;

                //      An option whose length runs off the end of the packet.
                packet[DHCP_HEAD + 4 + 1] = 200;
                check("an option longer than the packet is refused",
                      dhcp_read(packet, at, 0xdeadbeef, hardware, address_of lease,
                                address_of kind) < 0);
        }

        check("a /16 mask is a /16", dhcp_prefix_of(0xffff0000) == 16);
        check("a /32 mask is a /32", dhcp_prefix_of(0xffffffff) == 32);
        check("no mask at all falls back to /24", dhcp_prefix_of(0) == 24);
}

b32 main(void)
{
        arithmetic();
        building();
        padding();
        talking();
        resolving();
        fetching();
        fetching_for_real();
        leasing();

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
