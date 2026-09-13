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
        if (!word)
                return false;
        positive length = string_length(word);
        positive full_length = string_length((string_address)full);
        return length >= least && length <= full_length &&
               !string_compare_max(word, (string_address)full, length);
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
        netlink_link address_to link;
        string_address found = netlink_link_name(header, address_of link);
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
        netlink_link address_to link;
        string_address name = netlink_link_name(header, address_of link);
        (void)context;

        if (!name)
                return true;

        string_format(net_out, "%p: ", (positive)link->index);
        writer_terminal_name(net_out, name);
        string_format(net_out, ": ");
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
        netlink_address address_to body;
        net_naming address_to naming = (net_naming address_to)context;
        positive size = 0;
        positive label_size = 0;
        p8 address_to held;
        string_address label;
        p8 written[32];
        p32 host;

        if (header->length < NETLINK_HEADER + sizeof(netlink_address))
                return true;

        body = (netlink_address address_to)((p8 address_to)header +
                                             NETLINK_HEADER);
        held = (p8 address_to)netlink_find(header, sizeof(netlink_address),
                                           IFA_LOCAL, address_of size);
        label = (string_address)netlink_find(header, sizeof(netlink_address),
                                             IFA_LABEL,
                                             address_of label_size);
        if (!label_size || !label || !memory_first_of(label, 0, label_size))
                label = null;

        if (!held || size != 4 || body->family != AF_INET)
                return true;

        host = network_order_32(address_to((p32 address_to)held));

        string_format(net_out, "%p: ", (positive)body->index);
        writer_terminal_name(net_out,
                             label ? label : (string_address) "?");
        string_format(net_out, "    inet %s/%p\n",
                      net_host_text(written, host), (positive)body->prefix);

        (void)naming;

        return true;
}

static bool net_route_line(netlink_header address_to header, address_any context)
{
        netlink_route address_to body;
        positive gateway_size = 0;
        positive destination_size = 0;
        positive out_size = 0;
        p8 address_to gateway;
        p8 address_to destination;
        p8 address_to out;
        p8 written[32];
        (void)context;

        if (header->length < NETLINK_HEADER + sizeof(netlink_route))
                return true;

        body = (netlink_route address_to)((p8 address_to)header +
                                           NETLINK_HEADER);
        gateway = (p8 address_to)netlink_find(
            header, sizeof(netlink_route), RTA_GATEWAY,
            address_of gateway_size);
        destination = (p8 address_to)netlink_find(
            header, sizeof(netlink_route), RTA_DST,
            address_of destination_size);
        out = (p8 address_to)netlink_find(
            header, sizeof(netlink_route), RTA_OIF, address_of out_size);

        if (body->family != AF_INET || body->table != RT_TABLE_MAIN)
                return true;

        /* Every IPv4 route value used below is exactly one p32.  A shorter
           attribute would read into padding or the next attribute; a longer
           one is not the route shape this formatter understands. */
        if ((gateway && gateway_size != 4) ||
            (destination && destination_size != 4) ||
            (out && out_size != 4))
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
                {
                        string_format(net_out, " dev ");
                        writer_terminal_name(net_out, name);
                }
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
                        file_name_message(net_out,
                                          (string_address) "host: ",
                                          net_word(2),
                                          (string_address) " is not an address\n");
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
                file_name_message(net_out, (string_address) "", net_word(1),
                                  (string_address) " has address ");
                string_format(net_out, "%s\n", net_host_text(written, found));
                break;
        case DNS_NO_SUCH_NAME:
                file_name_message(net_out, (string_address) "host: ",
                                  net_word(1),
                                  (string_address) ": no such name\n");
                break;
        case DNS_NO_ADDRESS:
                file_name_message(net_out, (string_address) "host: ",
                                  net_word(1),
                                  (string_address) " exists but has no address\n");
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
        case DNS_NO_RANDOM:
                string_format(net_out, "host: secure randomness is not ready\n");
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
        bool tls = false;
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

        status = http_split_into(net_word(1), name, sizeof name, address_of port,
                                 address_of path, address_of tls);

        if (tls)
        {
                string_format(net_out, "fetch: https is not implemented; this speaks "
                                   "http only. use wget\n");
                net_flush();
                return 1;
        }

        if (status < 0)
        {
                file_name_message(
                    net_out, (string_address) "fetch: ", net_word(1),
                    (string_address) " is not a url this understands\n");
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
                        file_name_message(net_out,
                                          (string_address) "fetch: cannot resolve ",
                                          name, (string_address) "\n");
                        net_flush();
                        return 1;
                }
        }

        status = http_get(host, port, name, path, address_of body, address_of code);

        if (status < 0)
        {
                if (status == HTTP_NO_ROUTE)
                        file_name_message(net_out,
                                          (string_address) "fetch: cannot reach ",
                                          name, (string_address) "\n");
                else if (status == HTTP_NO_REPLY)
                        file_name_message(net_out,
                                          (string_address) "fetch: no reply from ",
                                          name, (string_address) "\n");
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


static const file_long wget_longs[] = {
    {(string_address) "output-document", 'O'},
    {(string_address) "quiet", 'q'},
    {(string_address) "help", 'h'},
    {(string_address) "version", 'V'},
    {(string_address) "no-check-certificate", 'K'},
    {null, 0},
};

/* A download and resolv.conf update share the same publication rule: write a
   fresh regular file through its exclusive descriptor, sync it, prove its
   directory entry while that descriptor remains open, then rename it over
   the destination in one operation. The parent directory
   remains pinned throughout, so renaming a command-line ancestor cannot send
   publication somewhere else. Failed staging is removed only through the
   still-open descriptor; an entry whose identity changed is retained. */
typedef struct
{
        bipolar directory;
        bipolar handle;
        p8 destination[SYSTEM_PATH_LEAF_ROOM];
        p8 temporary[SYSTEM_PATH_LEAF_ROOM];
} net_staging;

static bipolar net_staging_open(net_staging address_to file,
                                string_address destination,
                                string_address marker,
                                positive marker_length,
                                positive mode)
{
        file->directory = system_open_parent_pinned(
            AT_FDCWD, destination, file->destination,
            sizeof(file->destination));
        file->handle = -1;
        file->temporary[0] = end;

        if (file->directory < 0)
                return file->directory;

        file->handle = file_temporary_open_at(
            file->directory, file->destination, file->temporary,
            sizeof(file->temporary), marker, marker_length, system_nonce(),
            128, mode);

        if (file->handle < 0)
        {
                bipolar failed = file->handle;
                system_close(file->directory);
                file->directory = -1;
                return failed;
        }

        return file->handle;
}

static bipolar net_staging_finish(net_staging address_to file, bool publish)
{
        bipolar failed = 0;

        if (publish)
                failed = system_call_1(syscall(fsync),
                                       (positive)file->handle);

        if (!publish)
                failed = system_path_remove_opened_at(
                    file->directory, file->temporary, file->handle, 0);

        if (publish && !failed)
                failed = file_temporary_publish_at(
                    file->directory, file->temporary, file->destination,
                    file->handle, 0);

        bipolar closed = system_close(file->handle);
        file->handle = -1;

        /* Once a synced file has been atomically published, a later close
           error cannot be rolled back without replacing a possibly changed
           destination. Before publication, close failure remains failure. */
        if (!publish && !failed && closed < 0)
                failed = closed;

        system_close(file->directory);
        file->directory = -1;

        return failed;
}

/*
        wget [ -q ] [ -O FILE ] [ --no-check-certificate ] URL

        BusyBox's subset: save the body under the URL's last component, or
        under -O, and follow redirects. HTTPS is the reason this exists;
        fetch remains the small plaintext tool.
*/
static b32 net_wget(void)
{
        file_taking taking = {
            .program = (string_address) "wget",
            .allowed = (string_address) "OqhVK",
            .valued = (string_address) "O",
            .longs = wget_longs,
        };
        string_address url;
        string_address output;
        p8 name[256];
        p8 leaf[256];
        string_address path;
        p16 port = 80;
        bool tls = false;
        bool quiet;
        bool check_cert;
        bipolar dest = -1;
        bipolar status;
        b32 code = 0;
        bool own_file = false;
        net_staging staged;

        if (!file_take(address_of taking))
                return 1;

        if (file_meta(address_of taking,
                      (string_address) "[-q] [-O FILE] [--no-check-certificate] URL",
                      log))
        {
                log_flush();
                return 0;
        }

        if (taking.first >= (positive)program_argument_count())
        {
                string_format(log_error, "wget: missing URL\n");
                string_format(log_error, "Usage: wget [-q] [-O FILE] "
                                         "[--no-check-certificate] URL\n");
                return 1;
        }

        if (taking.first + 1 < (positive)program_argument_count())
        {
                file_name_message(
                    log_error, (string_address) "wget: extra operand '",
                    program_argument((b32)(taking.first + 1)),
                    (string_address) "'\n");
                return 1;
        }

        url = program_argument((b32)taking.first);
        quiet = (taking.flags & FILE_FLAG('q')) != 0;
        check_cert = (taking.flags & FILE_FLAG('K')) == 0;
        output = file_option_value(address_of taking, 'O');

        status = http_split_into(url, name, sizeof name, address_of port,
                                 address_of path, address_of tls);
        if (status)
        {
                file_name_message(
                    log_error, (string_address) "wget: ", url,
                    (string_address) " is not a url this understands\n");
                return 1;
        }

        if (output && string_equals(output, (string_address) "-"))
                dest = 1;
        else
        {
                if (!output)
                {
                        http_url_leaf(path, leaf, sizeof leaf);
                        output = leaf;
                }
                dest = net_staging_open(
                    address_of staged, output,
                    (string_address) ".moonwater-wget-",
                    sizeof(".moonwater-wget-") - 1, 0644);
                if (dest < 0)
                {
                        file_name_message(log_error,
                                          (string_address) "wget: cannot write ",
                                          output, (string_address) "\n");
                        return 1;
                }
                own_file = true;
        }

        if (!quiet)
        {
                file_name_pair_message(log_error, (string_address) "", url,
                                       (string_address) "\nSaving to: '", output,
                                       (string_address) "'\n");
        }

        status = http_fetch_to(url, dest, check_cert, address_of code);

        if (status)
        {
                if (own_file)
                {
                        if (net_staging_finish(address_of staged, false) < 0)
                                file_name_message(
                                    log_error,
                                    (string_address) "wget: incomplete staging file retained beside '",
                                    output, (string_address) "'\n");
                }
                if (status == HTTP_NO_HOST)
                        file_name_message(log_error,
                                          (string_address) "wget: cannot resolve ",
                                          name, (string_address) "\n");
                else if (status == HTTP_NO_ROUTE)
                        file_name_message(log_error,
                                          (string_address) "wget: cannot reach ",
                                          name, (string_address) "\n");
                else if (status == HTTP_TLS)
                        string_format(log_error, "wget: TLS handshake failed\n");
                else if (status == HTTP_DOWNGRADE)
                        string_format(log_error,
                                      "wget: refused an HTTPS to HTTP redirect\n");
                else if (status == HTTP_REDIRECTS)
                        string_format(log_error, "wget: too many redirects\n");
                else if (status == HTTP_NO_REPLY)
                        file_name_message(log_error,
                                          (string_address) "wget: no reply from ",
                                          name, (string_address) "\n");
                else
                        string_format(log_error, "wget: download failed\n");
                return 1;
        }

        if (code >= 400)
        {
                if (own_file)
                {
                        if (net_staging_finish(address_of staged, false) < 0)
                                file_name_message(
                                    log_error,
                                    (string_address) "wget: rejected response retained beside '",
                                    output, (string_address) "'\n");
                }
                string_format(log_error, "wget: server returned %p\n",
                              (positive)code);
                return 1;
        }

        if (own_file && net_staging_finish(address_of staged, true) < 0)
        {
                file_name_message(
                    log_error, (string_address) "wget: cannot publish ", output,
                    (string_address) "; staging file retained\n");
                return 1;
        }

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
static bipolar net_write_resolv_to(string_address path, p32 nameserver)
{
        p8 line[64];
        positive used = 11;
        net_staging staged;
        bipolar handle;

        memory_copy(line, "nameserver ", 11);
        used += host_into(line + used, DNS_FALLBACK);
        line[used++] = '\n';

        if (nameserver && nameserver != DNS_FALLBACK)
        {
                memory_copy(line + used, "nameserver ", 11);
                used += 11;
                used += host_into(line + used, nameserver);
                line[used++] = '\n';
        }

        handle = net_staging_open(
            address_of staged, path, (string_address) ".moonwater-resolv-",
            sizeof(".moonwater-resolv-") - 1, 0644);

        if (handle < 0)
                return handle;

        if (system_write_all((positive)handle, line, used) != used)
        {
                net_staging_finish(address_of staged, false);
                return -ERROR_INPUT_OUTPUT;
        }

        return net_staging_finish(address_of staged, true);
}

static bipolar net_write_resolv(p32 nameserver)
{
        return net_write_resolv_to((string_address) "/etc/resolv.conf",
                                   nameserver);
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
        bool lost;
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

/* Deleting state which the kernel already discarded is the same outcome as
   deleting it ourselves. A vanished interface reports ENODEV; absent
   addresses and routes are reported as ENOENT or ESRCH. */
static bool net_change_gone(bipolar status)
{
        return status >= 0 || status == -ERROR_NO_ENTRY ||
               status == -ERROR_NO_PROCESS || status == -ERROR_NO_DEVICE;
}

static bool net_holds_address(const net_holding address_to held, p32 index,
                              const dhcp_lease address_to lease)
{
        return held && held->index == index &&
               held->lease.address == lease->address &&
               dhcp_prefix_of(held->lease.mask) == dhcp_prefix_of(lease->mask);
}

static bool net_holds_route(const net_holding address_to held, p32 index,
                            const dhcp_lease address_to lease)
{
        if (!held)
                return !lease->router;

        return held->lease.router == lease->router &&
               (!lease->router || held->index == index);
}

/* Unsigned subtraction deliberately treats a clock failure or regression as
   an expired lease: keeping an address past the server's deadline can create
   an address collision, while releasing it merely requires reacquisition. */
static bool net_lease_expired_at(const net_holding address_to held,
                                 positive now)
{
        if (!held || !held->index || !held->lease.seconds)
                return false;

        return !now || !held->taken || now < held->taken ||
               now - held->taken >= held->lease.seconds;
}

static positive net_lease_due_in(const net_holding address_to held,
                                 positive now)
{
        positive half;
        positive gone;

        if (!held || !held->index || !held->lease.seconds)
                return 0;
        if (net_lease_expired_at(held, now))
                return 1;

        half = held->lease.seconds / 2;
        gone = now - held->taken;
        return gone >= half ? 1 : half - gone;
}

static bipolar net_holding_release(b32 handle, net_holding address_to held)
{
        bipolar failed = 0;

        if (!held || !held->index)
                return 0;

        if (held->lease.router)
        {
                bipolar status = netlink_route_delete(handle, 0, 0,
                                                       held->lease.router,
                                                       held->index);
                if (!net_change_gone(status))
                        failed = status;
        }

        if (held->lease.address)
        {
                bipolar status = netlink_address_delete(
                    handle, held->index, held->lease.address,
                    dhcp_prefix_of(held->lease.mask));
                if (!net_change_gone(status) && !failed)
                        failed = status;
        }

        /* Once the address is no longer ours, a DHCP-provided resolver is no
           longer ours either.  Keep the always-available fallback as the
           complete resolver file, using the same checked atomic publication
           path as lease installation. */
        {
                bipolar status = net_write_resolv(0);

                if (status < 0 && !failed)
                        failed = status;
        }

        if (!failed)
                memory_fill(held, 0, sizeof(*held));
        return failed;
}

static fn net_rollback_record(bipolar status,
                              bipolar address_to first)
{
        if (!net_change_gone(status) && !*first)
                *first = status;
}

/* Put the kernel back on the previous lease after any later step fails. The
   old address is restored before its route; a new route is removed before
   its address. Failures are remembered, but every independent cleanup is
   still attempted so one refusal cannot strand the rest. */
static bipolar net_lease_rollback(
    b32 handle, const net_holding address_to previous,
    p32 index, const dhcp_lease address_to lease,
    bool address_changed, bool route_changed)
{
        bipolar failed = 0;

        if (address_changed && previous)
        {
                bipolar status = netlink_address_add(
                    handle, previous->index, previous->lease.address,
                    dhcp_prefix_of(previous->lease.mask));
                if (status < 0 && !failed)
                        failed = status;
        }

        if (route_changed)
        {
                if (previous && previous->lease.router)
                {
                        bipolar status = netlink_route_add(
                            handle, 0, 0, previous->lease.router,
                            previous->index);
                        if (status < 0 && !failed)
                                failed = status;

                        if (lease->router &&
                            (lease->router != previous->lease.router ||
                             index != previous->index))
                                net_rollback_record(
                                    netlink_route_delete(handle, 0, 0,
                                                         lease->router, index),
                                    address_of failed);
                }
                else if (lease->router)
                        net_rollback_record(
                            netlink_route_delete(handle, 0, 0,
                                                 lease->router, index),
                            address_of failed);
        }

        /* address_changed includes a prefix-only replacement.  The newly
           installed address is therefore always distinct from the previous
           kernel object and must be removed on rollback. */
        if (address_changed)
                net_rollback_record(
                    netlink_address_delete(handle, index, lease->address,
                                           dhcp_prefix_of(lease->mask)),
                    address_of failed);

        return failed;
}

static b32 net_apply_lease(b32 handle, p32 index, string_address name,
                           p8 address_to hardware,
                           const dhcp_lease address_to lease,
                           net_holding address_to held, bool announce)
{
        net_holding previous_value = {0};
        const net_holding address_to previous =
            held && held->index ? address_of previous_value : null;
        bool address_changed;
        bool route_changed;
        bool address_applied = false;
        bool route_applied = false;
        string_address doing = null;
        bipolar status = 0;
        p8 written[32];

        if (previous)
                previous_value = *held;

        address_changed = !net_holds_address(previous, index, lease);
        route_changed = !net_holds_route(previous, index, lease);

        if (address_changed)
        {
                status = netlink_address_add(handle, index, lease->address,
                                             dhcp_prefix_of(lease->mask));
                if (status < 0)
                {
                        doing = (string_address) "addr add";
                        goto failed;
                }
                address_applied = true;
        }

        if (route_changed && lease->router)
        {
                status = netlink_route_add(handle, 0, 0, lease->router, index);
                if (status < 0)
                {
                        doing = (string_address) "route add";
                        goto failed;
                }
                route_applied = true;
        }

        if (route_changed && previous && previous->lease.router)
        {
                status = netlink_route_delete(handle, 0, 0,
                                              previous->lease.router,
                                              previous->index);
                if (!net_change_gone(status))
                {
                        doing = (string_address) "old route delete";
                        goto failed;
                }
                route_applied = true;
        }

        /* A prefix is part of an address object's identity.  Since
           address_changed compares index, host and prefix, every previous
           object in this branch must be removed, including an otherwise
           identical address whose mask changed. */
        if (address_changed && previous)
        {
                status = netlink_address_delete(
                    handle, previous->index, previous->lease.address,
                    dhcp_prefix_of(previous->lease.mask));
                if (!net_change_gone(status))
                {
                        doing = (string_address) "old addr delete";
                        goto failed;
                }
        }

        status = net_write_resolv(lease->nameserver);
        if (status < 0)
        {
                doing = (string_address) "write resolv.conf";
                goto failed;
        }

        if (announce)
        {
                string_format(net_out, "ip: %s/%p on ",
                              net_host_text(written, lease->address),
                              (positive)dhcp_prefix_of(lease->mask));
                writer_terminal_name(net_out, name);
                string_format(net_out, "\n");

                if (lease->router)
                        string_format(net_out, "ip: default via %s\n",
                                      net_host_text(written, lease->router));

                string_format(net_out, "ip: nameserver %s",
                              net_host_text(written, DNS_FALLBACK));

                if (lease->nameserver && lease->nameserver != DNS_FALLBACK)
                        string_format(net_out, ", then %s",
                                      net_host_text(written,
                                                    lease->nameserver));

                string_format(net_out, "\n");
        }

        if (held)
        {
                net_holding next = {.index = index, .lease = *lease,
                                    .taken = net_seconds()};
                string_copy_max_end(next.name, name, IFNAME_SIZE - 1);
                memory_copy(next.hardware, hardware, 6);
                *held = next;
        }

        net_flush();
        return 0;

failed:
        {
                bipolar rollback = net_lease_rollback(
                    handle, previous, index, lease,
                    address_applied, route_applied);

                if (rollback < 0)
                {
                        if (held)
                                held->lost = true;
                        net_refused((string_address) "lease rollback",
                                    rollback);
                }
        }

        return net_refused(doing, status);
}

static b32 net_auto(b32 handle, net_holding address_to held)
{
        netlink_search search;
        dhcp_lease lease;
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
                file_name_message(net_out, (string_address) "ip: ",
                                  search.name,
                                  (string_address) " has no hardware address\n");
                net_flush();
                return 1;
        }

        file_name_message(net_out, (string_address) "ip: using ",
                          search.name, (string_address) "\n");

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

        return net_apply_lease(handle, search.index, search.name,
                               search.hardware, address_of lease, held, true);
}

static b32 net_reconfigure(b32 handle, net_holding address_to held)
{
        if (held && held->lost)
        {
                bipolar status = net_holding_release(handle, held);

                if (status < 0)
                        return net_refused((string_address) "lease release",
                                           status);
        }

        return net_auto(handle, held);
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

/* Remember every carrier transition, but reconfigure only when no lease is
   active or its interface actually loses carrier. A newly probed down link
   is actionable while unconfigured; a second live interface is not. */
static bool net_link_news(p32 index, p32 flags, net_holding address_to held)
{
        net_state address_to entry;
        positive at;

        for (at = 0; at < net_state_count; at++)
        {
                entry = ((net_state address_to)net_states.bytes) + at;

                if (entry->index != index)
                        continue;

                if (((entry->flags ^ flags) & IFF_RUNNING) == 0)
                        return false;

                entry->flags = flags;

                goto changed;
        }

        if (!net_room(address_of net_states,
                      (net_state_count + 1) * sizeof(net_state)))
                return false;

        entry = ((net_state address_to)net_states.bytes) + net_state_count++;
        entry->index = index;
        entry->flags = flags;

changed:
        if (held && held->index == index && !(flags & IFF_RUNNING))
                held->lost = true;
        return !held || held->index == 0 || held->lost;
}

static bool net_link_removed(p32 index, net_holding address_to held)
{
        net_state address_to states = (net_state address_to)net_states.bytes;

        /* Forget the carrier snapshot as well as the lease.  Interface
           indexes may be reused, and retaining the deleted device's flags
           could suppress the replacement device's first event. */
        for (positive at = 0; at < net_state_count; at++)
                if (states[at].index == index)
                {
                        net_state_count--;
                        if (at != net_state_count)
                                states[at] = states[net_state_count];
                        break;
                }

        if (!held || held->index != index)
                return false;
        held->lost = true;
        return true;
}

/* Link multicast records are untrusted variable-length netlink messages.
   DELLINK needs only the fixed interface index; it must not consult flags or
   optional attributes from a device which no longer exists. */
static bool net_link_event(netlink_header address_to header,
                           net_holding address_to held)
{
        netlink_link address_to link;

        if (!header || header->port ||
            header->length < NETLINK_HEADER + sizeof(netlink_link))
                return false;

        link = (netlink_link address_to)((p8 address_to)header +
                                         NETLINK_HEADER);
        if (header->type == RTM_DELLINK)
                return net_link_removed(link->index, held);
        if (header->type != RTM_NEWLINK || (link->flags & IFF_LOOPBACK))
                return false;
        return net_link_news(link->index, link->flags, held);
}

static b32 net_watch(void)
{
        netlink_buffer message = {0};
        net_holding held;
        bipolar events;
        bipolar handle;

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
                string_format(net_out, "ip: %s\n", (string_address) "cannot listen for link changes");
                net_flush();
                return 1;
        }

        //      Configure whatever is already plugged in before waiting for
        //      anything to change, or a machine that boots with its cable in
        //      would wait forever for an event that already happened.
        handle = netlink_open_groups(0);

        memory_fill(address_of held, 0, sizeof held);

        if (handle >= 0)
        {
                net_reconfigure((b32)handle, address_of held);
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
                        bipolar ready;

                        if (held.index && held.lease.seconds)
                                due = net_lease_due_in(address_of held,
                                                       net_seconds());

                        ready = network_wait_readable(
                            events, due ? due : 3600, 0);
                        if (ready < 0)
                        {
                                /* A signal does not turn the following
                                   receive into an unbounded wait: recompute
                                   the lease deadline and poll again. Other
                                   descriptor failures terminate the watcher. */
                                if (ready == NETWORK_INTERRUPTED)
                                        continue;
                                break;
                        }

                        if (!ready)
                        {
                                //      Nothing arrived, so this is the lease
                                //      falling due. Ask to keep what we have;
                                //      if the server will not say yes, start
                                //      over, which is what a client does when
                                //      the lease finally runs out anyway.
                                if (!held.index || !held.lease.seconds)
                                        continue;

                                /* A renewal timeout must not extend a lease.
                                   At the actual deadline first remove the old
                                   address, route and resolver, then discover
                                   from a clean state. */
                                if (net_lease_expired_at(address_of held,
                                                         net_seconds()))
                                {
                                        held.lost = true;
                                        handle = netlink_open_groups(0);

                                        if (handle >= 0)
                                        {
                                                net_reconfigure(
                                                    (b32)handle,
                                                    address_of held);
                                                socket_close((b32)handle);
                                        }
                                        continue;
                                }

                                dhcp_lease renewed = held.lease;
                                bipolar renewal = dhcp_renew(
                                    held.name, held.hardware,
                                    address_of renewed);

                                if (renewal == DHCP_OK)
                                {
                                        handle = netlink_open_groups(0);

                                        if (handle >= 0)
                                        {
                                                if (!net_apply_lease(
                                                        (b32)handle,
                                                        held.index, held.name,
                                                        held.hardware,
                                                        address_of renewed,
                                                        address_of held,
                                                        false))
                                                {
                                                        file_name_message(
                                                            net_out,
                                                            (string_address) "ip: lease renewed on ",
                                                            held.name,
                                                            (string_address) "\n");
                                                        net_flush();
                                                }
                                                socket_close((b32)handle);
                                        }
                                        continue;
                                }

                                if (renewal == DHCP_REFUSED ||
                                    net_lease_expired_at(address_of held,
                                                         net_seconds()))
                                        held.lost = true;

                                handle = netlink_open_groups(0);

                                if (handle >= 0)
                                {
                                        net_reconfigure((b32)handle,
                                                        address_of held);
                                        socket_close((b32)handle);
                                }

                                continue;
                        }
                }

                got = netlink_receive((b32)events, address_of message, null);

                if (got < 0)
                        break;

                while (at + NETLINK_HEADER <= message.used)
                {
                        bool interesting = false;

                        header = (netlink_header address_to)(message.bytes + at);

                        if (header->length < NETLINK_HEADER ||
                            at + header->length > message.used)
                                break;

                        interesting = net_link_event(header,
                                                     address_of held);

                        at += netlink_align(header->length);

                        if (!interesting)
                                continue;

                        handle = netlink_open_groups(0);

                        if (handle < 0)
                                continue;

                        net_reconfigure((b32)handle, address_of held);
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

        handle = netlink_open_groups(0);

        if (handle < 0)
        {
                string_format(net_out, "ip: %s\n", (string_address) "cannot open a netlink socket");
                net_flush();
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
                else if (net_word_is(verb, "set", 3) && net_words() == 5 &&
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
                        string_format(net_out, "ip: %s\n", (string_address) "link: only 'show' and 'set NAME up'");
                        net_flush();
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
                else if (net_word_is(verb, "add", 1) && net_words() == 6 &&
                         net_word_is(net_word(4), "dev", 3))
                {
                        p32 host = 0;
                        p8 bits = 32;
                        bipolar index;

                        if (!net_split_prefix(net_word(3), address_of host,
                                              address_of bits))
                        {
                                string_format(net_out, "ip: %s\n", (string_address) "addr add: not an address");
                                net_flush();
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
                        string_format(net_out, "ip: %s\n", (string_address)
                                                             "addr: only 'show' and 'add A.B.C.D/N dev NAME'");
                        net_flush();
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
                else if (net_word_is(verb, "add", 1) &&
                         (net_words() == 6 || (net_words() == 8 &&
                          net_word_is(net_word(6), "dev", 3))) &&
                         net_word_is(net_word(3), "default", 3) &&
                         net_word_is(net_word(4), "via", 3))
                {
                        bipolar gateway = string_to_host(net_word(5));
                        bipolar index = 0;

                        if (gateway < 0)
                        {
                                string_format(net_out, "ip: %s\n", (string_address) "route add: not an address");
                                net_flush();
                                status = 1;
                        }
                        else if (net_words() == 8 &&
                                 (index = net_index_of((b32)handle, net_word(7), null)) < 0)
                                status = net_refused((string_address) "route add", index);
                        else
                        {
                                bipolar done = netlink_route_add(
                                    (b32)handle, 0, 0, (p32)gateway, (p32)index);

                                if (done < 0)
                                        status = net_refused((string_address) "route add", done);
                        }
                }
                else
                {
                        string_format(net_out, "ip: %s\n", (string_address)
                                                             "route: only 'show' and 'add default via A.B.C.D'");
                        net_flush();
                        status = 1;
                }
        }
        else
        {
                string_format(net_out, "ip: %s\n", (string_address) "unknown object; try link, addr or route");
                net_flush();
                status = 1;
        }

        log_flush();
        socket_close((b32)handle);

        return status;
}

#endif // STANDARD_MODERN_C_SHELL_NET
