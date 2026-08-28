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

//      argv, declared the way builtin.c declares it: this file is
//      pulled in before the definition further down shell.c.
string_address address_to shell_argv;
positive shell_argc;

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

static fn net_complain(string_address message)
{
        string_format(log, "ip: %s\n", message);
        log_flush();
}

//      The errno the kernel gave, said as plainly as this can say it.
static b32 net_refused(string_address doing, bipolar status)
{
        if (status == -1)
                string_format(log, "ip: %s: not permitted\n", doing);
        else if (status == -19)
                string_format(log, "ip: %s: no such device\n", doing);
        else if (status == -101)
                string_format(log, "ip: %s: network is unreachable\n", doing);
        else if (status == -13)
                string_format(log, "ip: %s: permission denied\n", doing);
        else
                string_format(log, "ip: %s: failed (%p)\n", doing, (positive)(-status));

        log_flush();

        return 1;
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

        string_format(log, "<");

        if (flags & IFF_UP)
        {
                string_format(log, "%sUP", between);
                between = (string_address) ",";
        }

        if (flags & IFF_BROADCAST)
        {
                string_format(log, "%sBROADCAST", between);
                between = (string_address) ",";
        }

        if (flags & IFF_LOOPBACK)
        {
                string_format(log, "%sLOOPBACK", between);
                between = (string_address) ",";
        }

        if (flags & IFF_RUNNING)
                string_format(log, "%sLOWER_UP", between);

        string_format(log, ">");
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

static string_address net_name_of(p32 index)
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

        string_format(log, "%p: %s: ", (positive)link->index, name);
        net_say_flags(link->flags);
        string_format(log, " state %s\n",
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
        positive length;
        p32 host;

        if (!held || size != 4 || body->family != AF_INET)
                return true;

        host = network_order_32(address_to((p32 address_to)held));
        length = host_into(written, host);
        written[length] = end;

        string_format(log, "%p: %s    inet %s/%p\n", (positive)body->index,
                      label ? label : (string_address) "?", written,
                      (positive)body->prefix);

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
        positive length;
        (void)context;

        if (body->family != AF_INET || body->table != RT_TABLE_MAIN)
                return true;

        if (destination)
        {
                length = host_into(written,
                                   network_order_32(address_to((p32 address_to)destination)));
                written[length] = end;
                string_format(log, "%s/%p", written, (positive)body->destination_bits);
        }
        else
        {
                string_format(log, "default");
        }

        if (gateway)
        {
                length = host_into(written,
                                   network_order_32(address_to((p32 address_to)gateway)));
                written[length] = end;
                string_format(log, " via %s", written);
        }

        if (out)
        {
                p32 index = address_to((p32 address_to)out);
                string_address name = net_name_of(index);

                if (name)
                        string_format(log, " dev %s", name);
                else
                        string_format(log, " dev %p", (positive)index);
        }

        string_format(log, "\n");

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
        positive length;
        bipolar server;
        bipolar status;

        if (shell_argc < 2)
        {
                string_format(log, "usage: host NAME [SERVER]\n");
                log_flush();
                return 1;
        }

        if (shell_argc > 2)
        {
                server = string_to_host(shell_argv[2]);

                if (server < 0)
                {
                        string_format(log, "host: %s is not an address\n", shell_argv[2]);
                        log_flush();
                        return 1;
                }
        }
        else
        {
                server = dns_server_from_file((string_address) "/etc/resolv.conf");

                if (server < 0)
                {
                        string_format(log, "host: no nameserver in /etc/resolv.conf, "
                                           "and none given\n");
                        log_flush();
                        return 1;
                }
        }

        status = dns_resolve((p32)server, shell_argv[1], address_of found, 5);

        switch (status)
        {
        case DNS_OK:
                length = host_into(written, found);
                written[length] = end;
                string_format(log, "%s has address %s\n", shell_argv[1], written);
                break;
        case DNS_NO_SUCH_NAME:
                string_format(log, "host: %s: no such name\n", shell_argv[1]);
                break;
        case DNS_NO_ADDRESS:
                string_format(log, "host: %s exists but has no address\n", shell_argv[1]);
                break;
        case DNS_NO_REPLY:
                string_format(log, "host: no reply from the nameserver\n");
                break;
        case DNS_REFUSED:
                string_format(log, "host: the nameserver refused the question\n");
                break;
        default:
                string_format(log, "host: the reply made no sense\n");
                break;
        }

        log_flush();

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

        if (shell_argc < 2)
        {
                string_format(log, "usage: fetch http://host[:port]/path\n");
                log_flush();
                return 1;
        }

        status = http_split(shell_argv[1], name, sizeof name, address_of port,
                            address_of path);

        if (status == HTTP_NOT_PLAIN)
        {
                string_format(log, "fetch: https is not implemented; this speaks "
                                   "http only\n");
                log_flush();
                return 1;
        }

        if (status < 0)
        {
                string_format(log, "fetch: %s is not a url this understands\n",
                              shell_argv[1]);
                log_flush();
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
                bipolar nameserver = dns_server_from_file(
                    (string_address) "/etc/resolv.conf");

                if (nameserver < 0)
                {
                        string_format(log, "fetch: no nameserver to ask about %s\n", name);
                        log_flush();
                        return 1;
                }

                if (dns_resolve((p32)nameserver, name, address_of host, 5) != DNS_OK)
                {
                        string_format(log, "fetch: cannot resolve %s\n", name);
                        log_flush();
                        return 1;
                }
        }

        status = http_get(host, port, name, path, address_of body, address_of code);

        if (status < 0)
        {
                if (status == HTTP_NO_ROUTE)
                        string_format(log, "fetch: cannot reach %s\n", name);
                else if (status == HTTP_NO_REPLY)
                        string_format(log, "fetch: no reply from %s\n", name);
                else
                        string_format(log, "fetch: the reply made no sense\n");

                log_flush();
                http_forget(address_of body);
                return 1;
        }

        if (code >= 300 && code < 400)
        {
                string_format(log, "fetch: %p, which is a redirect this does not "
                                   "follow\n", (positive)code);
                log_flush();
                http_forget(address_of body);
                return 1;
        }

        if (code >= 400)
        {
                string_format(log, "fetch: the server answered %p\n", (positive)code);
                log_flush();
                http_forget(address_of body);
                return 1;
        }

        if (body.used)
                system_write_all(1, body.bytes, body.used);

        http_forget(address_of body);

        return 0;
}

static b32 net_ip(void)
{
        bipolar handle;
        string_address object = shell_argc > 1 ? shell_argv[1] : null;
        string_address verb = shell_argc > 2 ? shell_argv[2] : null;
        b32 status = 0;

        if (!object || net_word_is(object, "help", 4))
        {
                string_format(log, "usage: ip link | addr | route\n");
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

        //      link ------------------------------------------------------
        if (net_word_is(object, "link", 1))
        {
                if (!verb || net_word_is(verb, "show", 1) || net_word_is(verb, "list", 1))
                {
                        if (netlink_dump((b32)handle, RTM_GETLINK, sizeof(netlink_link),
                                         AF_UNSPEC, net_link_line, null) < 0)
                                status = net_refused((string_address) "link show", -1);
                }
                else if (net_word_is(verb, "set", 3) && shell_argc > 4 &&
                         net_word_is(shell_argv[4], "up", 2))
                {
                        bipolar index = net_index_of((b32)handle, shell_argv[3], null);

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
                else if (net_word_is(verb, "add", 1) && shell_argc > 5 &&
                         net_word_is(shell_argv[4], "dev", 3))
                {
                        p32 host = 0;
                        p8 bits = 32;
                        bipolar index;

                        if (!net_split_prefix(shell_argv[3], address_of host,
                                              address_of bits))
                        {
                                net_complain((string_address) "addr add: not an address");
                                status = 1;
                        }
                        else if ((index = net_index_of((b32)handle, shell_argv[5], null)) < 0)
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
                else if (net_word_is(verb, "add", 1) && shell_argc > 5 &&
                         net_word_is(shell_argv[3], "default", 3) &&
                         net_word_is(shell_argv[4], "via", 3))
                {
                        bipolar gateway = string_to_host(shell_argv[5]);
                        p32 index = 0;

                        if (gateway < 0)
                        {
                                net_complain((string_address) "route add: not an address");
                                status = 1;
                        }
                        else
                        {
                                bipolar done;

                                if (shell_argc > 7 && net_word_is(shell_argv[6], "dev", 3))
                                {
                                        bipolar found = net_index_of((b32)handle,
                                                                     shell_argv[7], null);

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
