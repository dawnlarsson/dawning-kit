/*
        Experimental C standard library

        ip -- links, addresses and routes

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_SHELL_NET
#define STANDARD_MODERN_C_SHELL_NET

#include "../net/netlink.c"
#include "../net/dns.c"
#include "../net/http.c"
#include "../net/dhcp.c"

/*
        The words this was called with come from the process, not from the
        shell's own table.

        A utility here is reached two ways: typed at a prompt, where the shell
        points program_argument at the words it just split, and exec'd by name
        -- which is how init starts `ip watch` -- where they are the real argv
        the kernel handed over. program_argument answers both; shell_argv only
        answers the first, and a tool reading it comes up with no arguments at
        all when something execs it.
*/
#define net_words() ((positive)program_argument_count())
#define net_word(at) program_argument((b32)(at))

/*
        The subset of ip that configures a machine.

                ip link                     what interfaces there are
                ip link set NAME up         bring one up
                ip addr                     what addresses they have
                ip addr add A.B.C.D/N dev NAME
                ip route                    what routes there are
                ip route add default via A.B.C.D [dev NAME]

        iproute2 accepts abbreviations of everything and a grammar that goes
        on for a manual. This takes the words it needs in the order iproute2
        takes them, accepts the usual short forms of the objects, and refuses
        anything else rather than guessing -- a networking command that half
        understands what it was told is worse than one that says it did not.

        The output is shaped like iproute2's because scripts read it with awk,
        not because anything here parses it back.
*/


/*
        Where this says what it did.

        Typed at a prompt, ip writes to the terminal like any other command.
        Started by init, it is a service, and a service that writes to the
        console is writing into whatever else is using it -- which is a real
        problem rather than an untidy one: the boot test reads that console
        back, and an asynchronous DHCP exchange lands wherever it lands.

        So the watcher writes to /dev/kmsg instead. printk serialises whole
        records, so a line from here can never appear in the middle of a line
        from somewhere else, and it ends up in dmesg where a system message
        belongs while still reaching the console. If /dev/kmsg will not open
        -- an older image without the node -- this falls back to writing the
        same words the ordinary way, which is worse but not silent.

        A record is one write, so the bytes are gathered until a newline
        rather than passed through as string_format produces them.
*/
static b32 net_kmsg_handle = -1;
static p8 net_kmsg_line[512];
static positive net_kmsg_used;

/*
        Every record says what level it is, and says 6.

        A write to /dev/kmsg with no level on the front is given the default
        one, which this kernel sets to 7. The console prints what is BELOW its
        own loglevel, also 7, so a message at 7 goes into the log and never
        appears -- which is exactly what happened: the machine configured
        itself perfectly and said nothing about it. 6 is KERN_INFO, which is
        what this is.
*/
#define NET_KMSG_LEVEL "<6>"
#define NET_KMSG_LEVEL_BYTES 3

static fn net_kmsg_begin(void)
{
        memory_copy(net_kmsg_line, NET_KMSG_LEVEL, NET_KMSG_LEVEL_BYTES);
        net_kmsg_used = NET_KMSG_LEVEL_BYTES;
}

static fn net_kmsg(address_any data, positive length)
{
        p8 address_to bytes = (p8 address_to)data;
        positive at;

        if (!length)
                length = string_length(bytes);

        if (net_kmsg_used < NET_KMSG_LEVEL_BYTES)
                net_kmsg_begin();

        for (at = 0; at < length; at++)
        {
                if (bytes[at] == '\n' || net_kmsg_used + 2 >= sizeof net_kmsg_line)
                {
                        if (net_kmsg_used > NET_KMSG_LEVEL_BYTES)
                                system_write_all((positive)net_kmsg_handle,
                                                 net_kmsg_line, net_kmsg_used);

                        net_kmsg_begin();

                        if (bytes[at] == '\n')
                                continue;
                }

                net_kmsg_line[net_kmsg_used++] = bytes[at];
        }
}

//      The terminal by default; the kernel log once init owns this.
static writer net_out = log;

static fn net_flush(void)
{
        if (net_out == log)
                log_flush();
}

static bool net_word_is(string_address word, const char *full, positive least)
{
        positive at = 0;

        if (!word)
                return false;

        while (word[at])
        {
                if (!full[at] || word[at] != (p8)full[at])
                        return false;

                at++;
        }

        return at >= least;
}

static COLD fn net_complain(string_address message)
{
        string_format(net_out, "ip: %s\n", message);
        net_flush();
}

//      The errno the kernel gave, said as plainly as this can say it.
static COLD b32 net_refused(string_address doing, bipolar status)
{
        if (status == -1)
                string_format(net_out, "ip: %s: not permitted\n", doing);
        else if (status == -19)
                string_format(net_out, "ip: %s: no such device\n", doing);
        else if (status == -101)
                string_format(net_out, "ip: %s: network is unreachable\n", doing);
        else if (status == -13)
                string_format(net_out, "ip: %s: permission denied\n", doing);
        else
                string_format(net_out, "ip: %s: failed (%p)\n", doing, (positive)(-status));

        net_flush();

        return 1;
}

/*
        One address written into the caller's bytes as text it can hand to %s.

        host_into answers with a length and not with a string, because it also
        fills a field in the middle of a longer line. Eight places here wanted
        the other thing, and every one of them wrote the terminator itself --
        eight chances to write it one byte early or one byte late, in a buffer
        whose length only the call site knows.
*/
static string_address net_host_text(p8 address_to into, p32 host)
{
        into[host_into(into, host)] = end;

        return (string_address)into;
}

/*
        A.B.C.D/N split into the two halves it is written as.

        A missing prefix is /32 for an address, which is what iproute2 does
        and is almost never what was meant -- but guessing /24 from the first
        octet is the classful arithmetic that stopped being true in 1993, so
        the answer is to be literal and let the user say.
*/
static bool net_split_prefix(string_address text, p32 address_to host,
                             p8 address_to bits)
{
        string_address slash = string_first_of(text, '/');
        p8 kept[64];
        bipolar parsed;
        positive length;

        length = slash ? (positive)(slash - text) : string_length(text);

        if (length >= sizeof(kept))
                return false;

        string_copy_max_end(kept, text, length);

        parsed = string_to_host(kept);

        if (parsed < 0)
                return false;

        address_to host = (p32)parsed;

        if (!slash)
        {
                address_to bits = 32;
                return true;
        }

        if (!string_get(slash + 1))
                return false;

        parsed = (bipolar)string_to_positive(slash + 1);

        if (parsed < 0 || parsed > 32)
                return false;

        address_to bits = (p8)parsed;

        return true;
}

// The flags of a link, in the shape iproute2 writes them.
static fn net_say_flags(p32 flags)
{
        string_address between = (string_address) "";

        string_format(net_out, "<");

        if (flags & IFF_UP)
        {
                string_format(net_out, "%sUP", between);
                between = (string_address) ",";
        }

        if (flags & IFF_BROADCAST)
        {
                string_format(net_out, "%sBROADCAST", between);
                between = (string_address) ",";
        }

        if (flags & IFF_LOOPBACK)
        {
                string_format(net_out, "%sLOOPBACK", between);
                between = (string_address) ",";
        }

        if (flags & IFF_RUNNING)
                string_format(net_out, "%sLOWER_UP", between);

        string_format(net_out, ">");
}

/*
        Which index is which name, gathered once.

        A route names its interface by index and wants printing by name, and
        the obvious answer -- look the name up while walking the routes -- is
        the one thing netlink will not do: a dump started inside a dump is
        refused with EBUSY, because the first is still in progress on the
        socket. So the links are walked first, into a table, and the route
        walk only reads it.
*/
typedef struct
{
        p32 index;
        p8 name[IFNAME_SIZE];
} net_name;

static netlink_buffer net_names;
static positive net_name_count;

static bool net_name_seen(netlink_header address_to header, address_any context)
{
        netlink_link address_to link = (netlink_link address_to)((p8 address_to)header +
                                                                 NETLINK_HEADER);
        string_address found = (string_address)netlink_find(header, sizeof(netlink_link),
                                                            IFLA_IFNAME, null);
        net_name address_to entry;
        (void)context;

        if (!found)
                return true;

        if (!net_room(address_of net_names,
                      (net_name_count + 1) * sizeof(net_name)))
                return false;

        entry = ((net_name address_to)net_names.bytes) + net_name_count;
        entry->index = link->index;
        string_copy_max_end(entry->name, found, IFNAME_SIZE - 1);
        net_name_count++;

        return true;
}

static fn net_names_gather(b32 handle)
{
        net_name_count = 0;
        netlink_dump(handle, RTM_GETLINK, sizeof(netlink_link), AF_UNSPEC,
                     net_name_seen, null);
}

static PURE string_address net_name_of(p32 index)
{
        positive at;

        for (at = 0; at < net_name_count; at++)
        {
                net_name address_to entry = ((net_name address_to)net_names.bytes) + at;

                if (entry->index == index)
                        return entry->name;
        }

        return null;
}

static bool net_link_line(netlink_header address_to header, address_any context)
{
        netlink_link address_to link = (netlink_link address_to)((p8 address_to)header +
                                                                 NETLINK_HEADER);
        positive size = 0;
        string_address name = (string_address)netlink_find(header, sizeof(netlink_link),
                                                           IFLA_IFNAME, address_of size);
        (void)context;

        if (!name)
                return true;

        string_format(net_out, "%p: %s: ", (positive)link->index, name);
        net_say_flags(link->flags);
        string_format(net_out, " state %s\n",
                      (link->flags & IFF_UP) ? (string_address) "UP"
                                             : (string_address) "DOWN");

        return true;
}

typedef struct
{
        b32 handle;
        p32 index;
        p8 name[IFNAME_SIZE];
} net_naming;

//      An address line wants the interface's name, and the address dump gives
//      only its index, so the name is looked up once per line rather than the
//      whole link table being held.
static bool net_address_line(netlink_header address_to header, address_any context)
{
        netlink_address address_to body =
            (netlink_address address_to)((p8 address_to)header + NETLINK_HEADER);
        net_naming address_to naming = (net_naming address_to)context;
        positive size = 0;
        p8 address_to held = (p8 address_to)netlink_find(header, sizeof(netlink_address),
                                                         IFA_LOCAL, address_of size);
        string_address label = (string_address)netlink_find(header, sizeof(netlink_address),
                                                            IFA_LABEL, null);
        p8 written[32];
        p32 host;

        if (!held || size != 4 || body->family != AF_INET)
                return true;

        host = network_order_32(address_to((p32 address_to)held));

        string_format(net_out, "%p: %s    inet %s/%p\n", (positive)body->index,
                      label ? label : (string_address) "?",
                      net_host_text(written, host), (positive)body->prefix);

        (void)naming;

        return true;
}

static bool net_route_line(netlink_header address_to header, address_any context)
{
        netlink_route address_to body =
            (netlink_route address_to)((p8 address_to)header + NETLINK_HEADER);
        p8 address_to gateway = (p8 address_to)netlink_find(header, sizeof(netlink_route),
                                                            RTA_GATEWAY, null);
        p8 address_to destination = (p8 address_to)netlink_find(
            header, sizeof(netlink_route), RTA_DST, null);
        p8 address_to out = (p8 address_to)netlink_find(header, sizeof(netlink_route),
                                                        RTA_OIF, null);
        p8 written[32];
        (void)context;

        if (body->family != AF_INET || body->table != RT_TABLE_MAIN)
                return true;

        if (destination)
                string_format(net_out, "%s/%p",
                              net_host_text(written, network_order_32(
                                  address_to((p32 address_to)destination))),
                              (positive)body->destination_bits);
        else
                string_format(net_out, "default");

        if (gateway)
                string_format(net_out, " via %s",
                              net_host_text(written, network_order_32(
                                  address_to((p32 address_to)gateway))));

        if (out)
        {
                p32 index = address_to((p32 address_to)out);
                string_address name = net_name_of(index);

                if (name)
                        string_format(net_out, " dev %s", name);
                else
                        string_format(net_out, " dev %p", (positive)index);
        }

        string_format(net_out, "\n");

        return true;
}

//      Whatever this machine calls its interface, without being told.
static bipolar net_index_of(b32 handle, string_address name, p8 address_to into)
{
        netlink_search search;
        bipolar status;

        memory_fill(address_of search, 0, sizeof search);

        if (name)
                search.wanted = name;
        else
                search.skip_loopback = true;

        status = netlink_link_find(handle, address_of search);

        if (status < 0)
                return status;

        if (into)
                string_copy_max_end(into, search.name, IFNAME_SIZE - 1);

        return (bipolar)search.index;
}


/*
        host NAME [SERVER]

        The server is normally the first nameserver line in /etc/resolv.conf.
        It can be given instead, which is the only way to ask anything on a
        machine that has no resolv.conf -- which the boot image does not, and
        which is exactly when someone is trying to find out whether the
        network works at all.
*/
static b32 net_host(void)
{
        p32 found = 0;
        p8 written[32];
        bipolar server;
        bipolar status;

        if (net_words() < 2)
        {
                string_format(net_out, "usage: host NAME [SERVER]\n");
                net_flush();
                return 1;
        }

        if (net_words() > 2)
        {
                server = string_to_host(net_word(2));

                if (server < 0)
                {
                        string_format(net_out, "host: %s is not an address\n", net_word(2));
                        net_flush();
                        return 1;
                }

                status = dns_resolve((p32)server, net_word(1), address_of found, 5);
        }
        else
        {
                status = dns_resolve_any((string_address) "/etc/resolv.conf",
                                         net_word(1), address_of found, 3);
        }

        switch (status)
        {
        case DNS_OK:
                string_format(net_out, "%s has address %s\n", net_word(1),
                              net_host_text(written, found));
                break;
        case DNS_NO_SUCH_NAME:
                string_format(net_out, "host: %s: no such name\n", net_word(1));
                break;
        case DNS_NO_ADDRESS:
                string_format(net_out, "host: %s exists but has no address\n", net_word(1));
                break;
        case DNS_NO_REPLY:
                string_format(net_out, "host: no reply from the nameserver\n");
                break;
        case DNS_NO_SERVER:
                //      Not a bad answer -- no way to ask at all. Before an
                //      address exists there is no route to a nameserver, and
                //      saying the reply made no sense would send somebody
                //      looking at the wrong end of it.
                string_format(net_out, "host: cannot reach a nameserver; "
                                   "is the network up? try: ip auto\n");
                break;
        case DNS_REFUSED:
                string_format(net_out, "host: the nameserver refused the question\n");
                break;
        default:
                string_format(net_out, "host: the reply made no sense\n");
                break;
        }

        net_flush();

        return status == DNS_OK ? 0 : 1;
}


/*
        fetch URL

        The body goes to standard output, so it redirects into a file or pipes
        into anything else the way every other tool here does. What went wrong
        goes to the log, which is where a script looking only at the bytes
        will not mistake it for content.
*/
static b32 net_fetch(void)
{
        p8 name[256];
        http_buffer body = {0};
        string_address path;
        p16 port = 80;
        p32 host = 0;
        bipolar server;
        bipolar status;
        b32 code = 0;

        if (net_words() < 2)
        {
                string_format(net_out, "usage: fetch http://host[:port]/path\n");
                net_flush();
                return 1;
        }

        status = http_split(net_word(1), name, sizeof name, address_of port,
                            address_of path);

        if (status == HTTP_NOT_PLAIN)
        {
                string_format(net_out, "fetch: https is not implemented; this speaks "
                                   "http only\n");
                net_flush();
                return 1;
        }

        if (status < 0)
        {
                string_format(net_out, "fetch: %s is not a url this understands\n",
                              net_word(1));
                net_flush();
                return 1;
        }

        //      A literal address needs no resolver, which is what makes the
        //      test able to fetch from a socket on loopback with no nameserver
        //      anywhere in sight.
        server = string_to_host(name);

        if (server >= 0)
        {
                host = (p32)server;
        }
        else
        {
                if (dns_resolve_any((string_address) "/etc/resolv.conf", name,
                                    address_of host, 3) != DNS_OK)
                {
                        string_format(net_out, "fetch: cannot resolve %s\n", name);
                        net_flush();
                        return 1;
                }
        }

        status = http_get(host, port, name, path, address_of body, address_of code);

        if (status < 0)
        {
                if (status == HTTP_NO_ROUTE)
                        string_format(net_out, "fetch: cannot reach %s\n", name);
                else if (status == HTTP_NO_REPLY)
                        string_format(net_out, "fetch: no reply from %s\n", name);
                else
                        string_format(net_out, "fetch: the reply made no sense\n");

                net_flush();
                http_forget(address_of body);
                return 1;
        }

        if (code >= 300 && code < 400)
        {
                string_format(net_out, "fetch: %p, which is a redirect this does not "
                                   "follow\n", (positive)code);
                net_flush();
                http_forget(address_of body);
                return 1;
        }

        if (code >= 400)
        {
                string_format(net_out, "fetch: the server answered %p\n", (positive)code);
                net_flush();
                http_forget(address_of body);
                return 1;
        }

        if (body.used)
                system_write_all(1, body.bytes, body.used);

        http_forget(address_of body);

        return 0;
}


/*
        /etc/resolv.conf, written with a resolver that is known to work.

        Cloudflare goes first, and the one the network handed out goes under
        it. A DHCP nameserver is usually the router in the corner, which is
        also the thing most likely to answer slowly, cache a stale record, or
        have been handed a captive portal's idea of the truth. Naming a public
        resolver first is what lets a machine work on a network whose own
        resolver does not.

        The network's own is still written, and is still asked, because it is
        the only one that knows the names inside the network. The resolver
        walks this list in order and does not stop at a public resolver saying
        it has never heard of something local.

        Order is the whole of the policy. Putting it here rather than in the
        resolver means changing which server is preferred is one line in a
        file, not a rebuild.
*/
static fn net_write_resolv(p32 nameserver)
{
        p8 line[64];
        positive used;
        b32 handle;

        //      O_WRONLY | O_CREAT | O_TRUNC, 0644
        handle = (b32)system_open_at_mode(AT_FDCWD,
                                     "/etc/resolv.conf",
                                    (1 | 0100 | 01000), 0644);

        if (handle < 0)
                return;

        used = 11;
        memory_copy(line, "nameserver ", 11);
        used += host_into(line + used, DNS_FALLBACK);
        line[used++] = '\n';
        system_write_all((positive)handle, line, used);

        if (nameserver && nameserver != DNS_FALLBACK)
        {
                used = 11;
                memory_copy(line, "nameserver ", 11);
                used += host_into(line + used, nameserver);
                line[used++] = '\n';
                system_write_all((positive)handle, line, used);
        }

        system_close(handle);
}

/*
        ip auto -- the whole thing, without being told anything.

        Find a link that is not loopback, bring it up, ask for a lease, and
        apply what comes back. This is what "the network just works" means in
        practice, and it is deliberately userspace rather than kernel: the
        kernel's own IP_PNP runs once, before init, for one interface, and
        cannot write a resolv.conf or try again when a cable is plugged in.

        Every step says what it did, because the failure that matters is not
        an error code but the machine coming up silently unreachable.
*/

/*
        What this machine is holding, and since when.

        A lease has a time on it and half of that is when a client should ask
        to keep what it has. Nothing else here needs a clock, so this is the
        only place one is read.
*/
typedef struct
{
        p32 index;
        p8 name[IFNAME_SIZE];
        p8 hardware[6];
        dhcp_lease lease;
        positive taken;
} net_holding;

#define NET_CLOCK_MONOTONIC 1

static positive net_seconds(void)
{
        timespec now = {0, 0};

        //      A clock that will not answer leaves every lease looking
        //      infinitely old, which renews immediately and often rather than
        //      never -- the safe way round.
        if (system_call_2(syscall(clock_gettime), NET_CLOCK_MONOTONIC,
                          (positive)address_of now))
                return 0;

        return (positive)now.tv_sec;
}

static b32 net_auto(b32 handle, net_holding address_to held)
{
        netlink_search search;
        dhcp_lease lease;
        p8 written[32];
        bipolar status;

        memory_fill(address_of search, 0, sizeof search);
        search.skip_loopback = true;

        if (netlink_link_find(handle, address_of search) < 0)
        {
                string_format(net_out, "ip: no interface to configure\n");
                net_flush();
                return 1;
        }

        if (!search.has_hardware)
        {
                string_format(net_out, "ip: %s has no hardware address\n", search.name);
                net_flush();
                return 1;
        }

        string_format(net_out, "ip: using %s\n", search.name);

        if (!(search.flags & IFF_UP))
        {
                status = netlink_link_up(handle, search.index);

                if (status < 0)
                        return net_refused((string_address) "link up", status);
        }

        string_format(net_out, "ip: asking for a lease\n");
        net_flush();

        status = dhcp_ask(search.name, search.hardware, address_of lease);

        if (status != DHCP_OK)
        {
                if (status == DHCP_REFUSED)
                        string_format(net_out, "ip: the server refused the request\n");
                else if (status == DHCP_NO_OFFER)
                        string_format(net_out, "ip: nobody offered a lease\n");
                else
                        string_format(net_out, "ip: could not ask for a lease\n");

                net_flush();
                return 1;
        }

        string_format(net_out, "ip: %s/%p on %s\n",
                      net_host_text(written, lease.address),
                      (positive)dhcp_prefix_of(lease.mask), search.name);

        status = netlink_address_add(handle, search.index, lease.address,
                                     dhcp_prefix_of(lease.mask));

        if (status < 0)
                return net_refused((string_address) "addr add", status);

        if (lease.router)
        {
                status = netlink_route_add(handle, 0, 0, lease.router, search.index);

                if (status < 0)
                        return net_refused((string_address) "route add", status);

                string_format(net_out, "ip: default via %s\n",
                              net_host_text(written, lease.router));
        }

        net_write_resolv(lease.nameserver);

        string_format(net_out, "ip: nameserver %s",
                      net_host_text(written, DNS_FALLBACK));

        if (lease.nameserver && lease.nameserver != DNS_FALLBACK)
        {
                string_format(net_out, ", then %s",
                              net_host_text(written, lease.nameserver));
        }

        string_format(net_out, "\n");

        if (held)
        {
                held->index = search.index;
                held->lease = lease;
                held->taken = net_seconds();
                string_copy_max_end(held->name, search.name, IFNAME_SIZE - 1);
                memory_copy(held->hardware, search.hardware, 6);
        }

        net_flush();

        return 0;
}


/*
        ip watch -- configure now, and again whenever the wires change.

        The kernel will tell you when a link gains or loses carrier if you ask
        it to: a netlink socket bound to the RTNLGRP_LINK multicast group
        receives an RTM_NEWLINK every time an interface changes state. No
        polling, no timer, nothing to tune -- the read simply blocks until
        something actually happens.

        What counts as "something" is deliberately narrow. An RTM_NEWLINK
        arrives for changes nobody cares about here, so only a change in
        IFF_RUNNING on a link that is not loopback causes anything: that is
        the kernel saying a cable was plugged in or pulled out. Everything
        else is read and dropped.

        On such a change the whole of ip auto runs again, which re-picks the
        best link rather than assuming the one that changed is the one to use.
        That is what makes the wired-to-wireless case work without any code
        that knows what wireless is: pull the cable, the wired link loses
        carrier, the walk picks whatever else has it.

        Re-running is safe to do at any time. Adding an address uses REPLACE
        and adding a route is idempotent, so a spurious run costs a DHCP
        exchange and changes nothing else.
*/
typedef struct
{
        p32 index;
        p32 flags;
} net_state;

static netlink_buffer net_states;
static positive net_state_count;

//      Whether anything worth reacting to happened to this link, remembering
//      its new state either way.
static bool net_link_news(p32 index, p32 flags, bool address_to had_carrier)
{
        net_state address_to entry;
        positive at;

        address_to had_carrier = false;

        for (at = 0; at < net_state_count; at++)
        {
                entry = ((net_state address_to)net_states.bytes) + at;

                if (entry->index != index)
                        continue;

                address_to had_carrier = (entry->flags & IFF_RUNNING) != 0;

                if (((entry->flags ^ flags) & IFF_RUNNING) == 0)
                        return false;

                entry->flags = flags;

                return true;
        }

        if (!net_room(address_of net_states,
                      (net_state_count + 1) * sizeof(net_state)))
                return false;

        entry = ((net_state address_to)net_states.bytes) + net_state_count++;
        entry->index = index;
        entry->flags = flags;

        //      A link nobody has seen before is news whatever state it is
        //      in. At boot the card is still being probed when init starts
        //      watching, so the interface appears a moment later, down and
        //      without carrier -- and it has to be acted on, because bringing
        //      it up is the very thing that would give it carrier. Waiting
        //      for a carrier event there waits forever.
        return true;
}

static b32 net_watch(void)
{
        netlink_buffer message = {0};
        net_holding held;
        bipolar events;
        bipolar handle;
        p32 configured = 0;

        //      O_WRONLY. A failure leaves the handle at -1 and net_out at
        //      log, which is exactly the old behaviour.
        net_kmsg_handle = (b32)system_open_at(AT_FDCWD,
                                              "/dev/kmsg", 1);

        if (net_kmsg_handle >= 0)
        {
                net_kmsg_begin();
                net_out = net_kmsg;
        }

        events = netlink_open_groups(RTNLGRP_LINK_MASK);

        if (events < 0)
        {
                net_complain((string_address) "cannot listen for link changes");
                return 1;
        }

        //      Configure whatever is already plugged in before waiting for
        //      anything to change, or a machine that boots with its cable in
        //      would wait forever for an event that already happened.
        handle = netlink_open();

        memory_fill(address_of held, 0, sizeof held);

        if (handle >= 0)
        {
                net_auto((b32)handle, address_of held);
                configured = held.index;
                socket_close((b32)handle);
        }

        for (;;)
        {
                netlink_header address_to header;
                positive at = 0;
                bipolar got;

                /*
                        Wait for a link to change, or for the lease to reach
                        the point where it should be renewed, whichever comes
                        first. Without the second, a machine that nobody
                        touches keeps an address the server has long since
                        considered free, and the first sign of trouble is
                        somebody else being handed it.
                */
                {
                        positive due = 0;

                        if (configured && held.lease.seconds)
                        {
                                positive half = held.lease.seconds / 2;
                                positive gone = net_seconds() - held.taken;

                                due = gone >= half ? 1 : half - gone;
                        }

                        if (network_wait_readable(events, due ? due : 3600,
                                                  0) == 0)
                        {
                                //      Nothing arrived, so this is the lease
                                //      falling due. Ask to keep what we have;
                                //      if the server will not say yes, start
                                //      over, which is what a client does when
                                //      the lease finally runs out anyway.
                                if (!configured || !held.lease.seconds)
                                        continue;

                                if (dhcp_renew(held.name, held.hardware,
                                               address_of held.lease) == DHCP_OK)
                                {
                                        held.taken = net_seconds();
                                        string_format(net_out,
                                                      "ip: lease renewed on %s\n",
                                                      held.name);
                                        net_flush();
                                        continue;
                                }

                                handle = netlink_open();

                                if (handle >= 0)
                                {
                                        net_auto((b32)handle, address_of held);
                                        configured = held.index;
                                        socket_close((b32)handle);
                                }

                                continue;
                        }
                }

                got = netlink_receive((b32)events, address_of message);

                if (got < 0)
                        break;

                while (at + NETLINK_HEADER <= message.used)
                {
                        netlink_link address_to link;
                        bool interesting = false;

                        header = (netlink_header address_to)(message.bytes + at);

                        if (header->length < NETLINK_HEADER ||
                            at + header->length > message.used)
                                break;

                        if (header->type == RTM_NEWLINK &&
                            header->length >= NETLINK_HEADER + sizeof(netlink_link))
                        {
                                link = (netlink_link address_to)(message.bytes + at +
                                                                 NETLINK_HEADER);

                                bool had_carrier = false;

                                if (!(link->flags & IFF_LOOPBACK) &&
                                    net_link_news(link->index, link->flags,
                                                  address_of had_carrier))
                                {
                                        /*
                                                Two things are worth acting
                                                on, and nothing else is.

                                                Anything at all, while nothing
                                                is configured: a card that has
                                                only just finished probing is
                                                the ordinary case at boot, and
                                                it arrives down and without
                                                carrier because bringing it up
                                                is what this is for.

                                                The configured link losing
                                                carrier: the cable came out,
                                                and whatever else has carrier
                                                should take over.

                                                A link gaining carrier while
                                                another already works is not
                                                news. Acting there would take
                                                a fresh lease for no reason,
                                                including on the link this had
                                                just brought up itself.
                                        */
                                        if (configured == 0)
                                                interesting = true;
                                        else if (link->index == configured &&
                                                 had_carrier &&
                                                 !(link->flags & IFF_RUNNING))
                                        {
                                                //      had_carrier matters.
                                                //      Bringing a link up
                                                //      produces an event
                                                //      before the carrier
                                                //      arrives, and reading
                                                //      that as the cable
                                                //      coming out made this
                                                //      configure itself
                                                //      twice at every boot.
                                                configured = 0;
                                                interesting = true;
                                        }
                                }
                        }

                        at += netlink_align(header->length);

                        if (!interesting)
                                continue;

                        handle = netlink_open();

                        if (handle < 0)
                                continue;

                        net_auto((b32)handle, address_of held);
                        configured = held.index;
                        socket_close((b32)handle);
                }
        }

        netlink_forget(address_of message);
        socket_close((b32)events);

        return 1;
}

static b32 net_ip(void)
{
        bipolar handle;
        string_address object = net_words() > 1 ? net_word(1) : null;
        string_address verb = net_words() > 2 ? net_word(2) : null;
        b32 status = 0;

        if (!object || net_word_is(object, "help", 4))
        {
                string_format(log, "usage: ip auto | link | addr | route\n");
                string_format(log, "       ip auto   find a link, bring it up, "
                                   "take a lease\n");
                string_format(log, "       ip watch  the same, now and whenever "
                                   "a cable changes\n");
                string_format(log, "       ip link set NAME up\n");
                string_format(log, "       ip addr add A.B.C.D/N dev NAME\n");
                string_format(log, "       ip route add default via A.B.C.D [dev NAME]\n");
                log_flush();
                return object ? 0 : 1;
        }

        handle = netlink_open();

        if (handle < 0)
        {
                net_complain((string_address) "cannot open a netlink socket");
                return 1;
        }

        //      auto ------------------------------------------------------
        if (net_word_is(object, "auto", 2))
        {
                status = net_auto((b32)handle, null);
        }
        //      watch -----------------------------------------------------
        else if (net_word_is(object, "watch", 1))
        {
                socket_close((b32)handle);
                return net_watch();
        }
        //      link ------------------------------------------------------
        else if (net_word_is(object, "link", 1))
        {
                if (!verb || net_word_is(verb, "show", 1) || net_word_is(verb, "list", 1))
                {
                        if (netlink_dump((b32)handle, RTM_GETLINK, sizeof(netlink_link),
                                         AF_UNSPEC, net_link_line, null) < 0)
                                status = net_refused((string_address) "link show", -1);
                }
                else if (net_word_is(verb, "set", 3) && net_words() > 4 &&
                         net_word_is(net_word(4), "up", 2))
                {
                        bipolar index = net_index_of((b32)handle, net_word(3), null);

                        if (index < 0)
                                status = net_refused((string_address) "link set", index);
                        else
                        {
                                bipolar done = netlink_link_up((b32)handle, (p32)index);

                                if (done < 0)
                                        status = net_refused((string_address) "link set",
                                                             done);
                        }
                }
                else
                {
                        net_complain((string_address) "link: only 'show' and 'set NAME up'");
                        status = 1;
                }
        }
        //      addr ------------------------------------------------------
        else if (net_word_is(object, "addr", 1) || net_word_is(object, "address", 1))
        {
                if (!verb || net_word_is(verb, "show", 1) || net_word_is(verb, "list", 1))
                {
                        if (netlink_dump((b32)handle, RTM_GETADDR, sizeof(netlink_address),
                                         AF_INET, net_address_line, null) < 0)
                                status = net_refused((string_address) "addr show", -1);
                }
                else if (net_word_is(verb, "add", 1) && net_words() > 5 &&
                         net_word_is(net_word(4), "dev", 3))
                {
                        p32 host = 0;
                        p8 bits = 32;
                        bipolar index;

                        if (!net_split_prefix(net_word(3), address_of host,
                                              address_of bits))
                        {
                                net_complain((string_address) "addr add: not an address");
                                status = 1;
                        }
                        else if ((index = net_index_of((b32)handle, net_word(5), null)) < 0)
                                status = net_refused((string_address) "addr add", index);
                        else
                        {
                                bipolar done = netlink_address_add((b32)handle, (p32)index,
                                                                   host, bits);

                                if (done < 0)
                                        status = net_refused((string_address) "addr add",
                                                             done);
                        }
                }
                else
                {
                        net_complain((string_address)
                                     "addr: only 'show' and 'add A.B.C.D/N dev NAME'");
                        status = 1;
                }
        }
        //      route -----------------------------------------------------
        else if (net_word_is(object, "route", 1))
        {
                if (!verb || net_word_is(verb, "show", 1) || net_word_is(verb, "list", 1))
                {
                        net_names_gather((b32)handle);

                        if (netlink_dump((b32)handle, RTM_GETROUTE, sizeof(netlink_route),
                                         AF_INET, net_route_line, null) < 0)
                                status = net_refused((string_address) "route show", -1);
                }
                else if (net_word_is(verb, "add", 1) && net_words() > 5 &&
                         net_word_is(net_word(3), "default", 3) &&
                         net_word_is(net_word(4), "via", 3))
                {
                        bipolar gateway = string_to_host(net_word(5));
                        p32 index = 0;

                        if (gateway < 0)
                        {
                                net_complain((string_address) "route add: not an address");
                                status = 1;
                        }
                        else
                        {
                                bipolar done;

                                if (net_words() > 7 && net_word_is(net_word(6), "dev", 3))
                                {
                                        bipolar found = net_index_of((b32)handle,
                                                                     net_word(7), null);

                                        if (found >= 0)
                                                index = (p32)found;
                                }

                                done = netlink_route_add((b32)handle, 0, 0, (p32)gateway,
                                                         index);

                                if (done < 0)
                                        status = net_refused((string_address) "route add",
                                                             done);
                        }
                }
                else
                {
                        net_complain((string_address)
                                     "route: only 'show' and 'add default via A.B.C.D'");
                        status = 1;
                }
        }
        else
        {
                net_complain((string_address) "unknown object; try link, addr or route");
                status = 1;
        }

        log_flush();
        socket_close((b32)handle);

        return status;
}

#endif // STANDARD_MODERN_C_SHELL_NET
