/*
        Experimental C standard library

        A resolver: a name, and the address behind it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_DNS
#define STANDARD_MODERN_C_NET_DNS

#include "wait.c"

/*
        The smallest resolver that is not wrong.

        DNS is a twelve byte header, a question written as length-prefixed
        labels, and answers in the same shape with a fixed ten byte tail. All
        of it is big endian, unlike netlink next door, which is the sort of
        neighbouring difference that produces a working program and a wrong
        one from the same afternoon.

        Three things a minimal client is tempted to skip and must not:

        Compression. Almost every real answer points into itself rather than
        spelling a name twice -- two octets whose top bits are both set, the
        remaining fourteen an offset from the start of the message. A parser
        that does not follow those reads garbage on nearly every reply. One
        that follows them without counting can be sent in a circle by two
        pointers aimed at each other, so the number of jumps is bounded by the
        size of the message, which no honest reply can exceed.

        Truncation. A reply too big for the buffer arrives shortened, and a
        shortened answer section is indistinguishable from a short one. The
        read asks for MSG_TRUNC so the kernel says the true size, and a reply
        bigger than what was read is refused rather than parsed.

        The question coming back. A reply is matched on its transaction id,
        and an id is sixteen bits, so it is not much of a check on its own.
        The question section is compared against the one that was asked, byte
        for byte, before a single answer is believed.

        rcode zero does not mean there is an address. A name that exists with
        no A record answers NOERROR with no answers at all, which is a
        different thing from the name not existing, and a resolver that
        conflates them reports the wrong reason forever.
*/

#define DNS_PORT 53
#define DNS_HEADER 12

#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_AAAA 28
#define DNS_CLASS_IN 1

#define DNS_FLAG_RESPONSE 0x8000
#define DNS_FLAG_TRUNCATED 0x0200
#define DNS_FLAG_RECURSE 0x0100
#define DNS_CODE_MASK 0x000f

#define DNS_MAX_MESSAGE 4096

//      Everything the caller may want to tell apart.
#define DNS_OK 0
#define DNS_NO_SERVER (-1)
#define DNS_NO_REPLY (-2)
#define DNS_MALFORMED (-3)
#define DNS_NO_SUCH_NAME (-4)
#define DNS_NO_ADDRESS (-5)
#define DNS_REFUSED (-6)

/*
        The name, as labels.

        "dawning.dev" becomes 7 d a w n i n g 3 d e v 0. A label may not
        exceed sixty three bytes -- the two high bits of a length are what
        marks a compression pointer, so a longer one would be unreadable
        rather than merely unusual -- and the whole name may not exceed 255.
*/
static bipolar dns_write_name(p8 address_to into, positive room, string_address name)
{
        positive used = 0;
        positive mark;
        positive length;

        while (string_get(name))
        {
                mark = used++;

                if (used >= room)
                        return DNS_MALFORMED;

                length = 0;

                while (string_get(name) && !string_is(name, '.'))
                {
                        if (used >= room || length >= 63)
                                return DNS_MALFORMED;

                        into[used++] = string_get(name);
                        name++;
                        length++;
                }

                if (!length)
                        return DNS_MALFORMED;

                into[mark] = (p8)length;

                if (string_is(name, '.'))
                        name++;
        }

        if (used + 1 > room || used + 1 > 255)
                return DNS_MALFORMED;

        into[used++] = 0;

        return (bipolar)used;
}

/*
        A name skipped over, following pointers but never in a circle.

        The answer is where the name ENDS in the message, which for a
        compressed name is two bytes on from where it began however far away
        the pointer led. Every jump is counted against the size of the whole
        message, which a legitimate reply cannot exceed.
*/
static PURE bipolar dns_skip_name(p8 address_to message, positive size, positive at)
{
        positive jumps = 0;
        bipolar ended = -1;

        for (;;)
        {
                p8 length;

                if (at >= size)
                        return DNS_MALFORMED;

                length = message[at];

                if ((length & 0xc0) == 0xc0)
                {
                        positive target;

                        if (at + 1 >= size)
                                return DNS_MALFORMED;

                        if (ended < 0)
                                ended = (bipolar)(at + 2);

                        target = (positive)(((length & 0x3f) << 8) | message[at + 1]);

                        //      A pointer must lead backwards into the message
                        //      that has already been seen. Anything else is a
                        //      loop dressed as an offset.
                        if (target >= at || ++jumps > size)
                                return DNS_MALFORMED;

                        at = target;
                        continue;
                }

                if (length & 0xc0)
                        return DNS_MALFORMED;

                at += 1 + length;

                if (!length)
                        return ended < 0 ? (bipolar)at : ended;
        }
}

//      A transaction id worth having. getrandom is the kernel's own, and the
//      clock is what answers when it is unavailable -- which is better than a
//      counter starting at one, and is why it is never simply a counter.
static p16 dns_transaction(void)
{
        p16 value = 0;

        if (system_call_3(syscall(getrandom), (positive)address_of value,
                          sizeof value, 1) == sizeof value)
                return value;

        return (p16)(get_cpu_time() ^ (get_cpu_time() >> 17));
}

/*
        The nameserver, out of resolv.conf.

        Only "nameserver A.B.C.D" lines. The wanted-th of them is returned, so
        a caller walks 0, 1, 2 until this answers negatively and the number of
        servers a machine may list has no ceiling. Options, search domains and
        IPv6 servers are read past rather than understood.
*/
static bipolar dns_server_at(string_address path, positive wanted)
{
        p8 text[4096];
        bipolar got;
        positive at = 0;

        got = file_slurp(path, text, sizeof text);

        if (got <= 0)
                return DNS_NO_SERVER;

        while (at < (positive)got)
        {
                positive line = at;
                p8 address_to newline = (p8 address_to)memory_first_of(
                    text + at, '\n', (positive)got - at);
                positive stop = newline ? (positive)(newline - text) : (positive)got;

                at = newline ? stop + 1 : stop;

                if (stop - line < 11)
                        continue;

                if (!string_compare_max(text + line, (string_address) "nameserver ", 11))
                {
                        p8 kept[64];
                        positive from = line + 11;
                        positive length;

                        from += string_span_max(text + from, stop - from,
                                                string_set_blanks);

                        length = stop - from;

                        while (length && (text[from + length - 1] == '\r' ||
                                          text[from + length - 1] == ' '))
                                length--;

                        if (length && length < sizeof(kept))
                        {
                                bipolar host;

                                string_copy_max_end(kept, text + from, length);
                                host = string_to_host(kept);

                                if (host >= 0)
                                {
                                        if (!wanted)
                                                return host;

                                        wanted--;
                                }
                        }
                }
        }

        return DNS_NO_SERVER;
}

/*
        One question asked, and the first address in the answer.

        The reply is read with a deadline rather than blocked on forever: a
        nameserver that does not answer is the ordinary case on a network that
        is not up yet, and a resolver that hangs there is worse than one that
        gives up and says so. The wait is a poll on the socket rather than a
        receive timeout, which keeps a timeval out of the assembly graph.
*/
static bipolar dns_resolve(p32 server, string_address name, p32 address_to found,
                           positive seconds)
{
        p8 request[DNS_MAX_MESSAGE];
        p8 reply[DNS_MAX_MESSAGE];
        p16 id = dns_transaction();
        bipolar handle;
        bipolar written;
        bipolar got;
        p16 flags;
        positive at;
        positive answers;
        positive question_length;

        written = dns_write_name(request + DNS_HEADER,
                                 sizeof(request) - DNS_HEADER - 4, name);

        if (written < 0)
                return DNS_MALFORMED;

        network_store_16(request, id);
        network_store_16(request + 2, DNS_FLAG_RECURSE);
        network_store_16(request + 4, 1);
        network_store_16(request + 6, 0);
        network_store_16(request + 8, 0);
        network_store_16(request + 10, 0);

        network_store_16(request + DNS_HEADER + written, DNS_TYPE_A);
        network_store_16(request + DNS_HEADER + written + 2, DNS_CLASS_IN);

        question_length = (positive)written + 4;

        handle = socket_new(AF_INET, SOCK_DGRAM, 0);

        if (handle < 0)
                return DNS_NO_SERVER;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(DNS_PORT),
            .host = network_order_32(server)};

        if (socket_connect((b32)handle, address_of where, sizeof where) < 0)
        {
                socket_close((b32)handle);
                return DNS_NO_SERVER;
        }

        if (socket_send((b32)handle, request, DNS_HEADER + question_length, 0, 0, 0) < 0)
        {
                socket_close((b32)handle);
                return DNS_NO_REPLY;
        }

        //      A resolver that blocks forever on a server that is not there
        //      looks like a hung machine rather than a network that is down.
        got = network_wait_readable(handle, seconds, 0);

        if (got <= 0)
        {
                socket_close((b32)handle);
                return DNS_NO_REPLY;
        }

        got = socket_receive((b32)handle, reply, sizeof reply, MSG_TRUNC, 0, 0);
        socket_close((b32)handle);

        if (got < DNS_HEADER)
                return DNS_NO_REPLY;

        //      MSG_TRUNC answers with the true length, so a reply that did
        //      not fit is refused rather than parsed as far as it got.
        if ((positive)got > sizeof(reply))
                return DNS_MALFORMED;

        if (network_load_16(reply) != id)
                return DNS_MALFORMED;

        flags = network_load_16(reply + 2);

        if (!(flags & DNS_FLAG_RESPONSE) || (flags & DNS_FLAG_TRUNCATED))
                return DNS_MALFORMED;

        if (network_load_16(reply + 4) != 1)
                return DNS_MALFORMED;

        //      The question, byte for byte as it was asked.
        if ((positive)got < DNS_HEADER + question_length ||
            memory_compare(reply + DNS_HEADER, request + DNS_HEADER, question_length))
                return DNS_MALFORMED;

        switch (flags & DNS_CODE_MASK)
        {
        case 0:
                break;
        case 3:
                return DNS_NO_SUCH_NAME;
        default:
                return DNS_REFUSED;
        }

        answers = network_load_16(reply + 6);
        at = DNS_HEADER + question_length;

        while (answers--)
        {
                bipolar next = dns_skip_name(reply, (positive)got, at);
                p16 kind;
                p16 class;
                p16 size;

                if (next < 0)
                        return DNS_MALFORMED;

                at = (positive)next;

                if (at + 10 > (positive)got)
                        return DNS_MALFORMED;

                kind = network_load_16(reply + at);
                class = network_load_16(reply + at + 2);
                size = network_load_16(reply + at + 8);
                at += 10;

                if (at + size > (positive)got)
                        return DNS_MALFORMED;

                //      A CNAME chain is walked by simply reading past it: the
                //      answer section carries the A record the alias leads to
                //      in the same reply, which is what recursion is for.
                if (kind == DNS_TYPE_A && class == DNS_CLASS_IN && size == 4)
                {
                        if (found)
                                address_to found = network_load_32(reply + at);

                        return DNS_OK;
                }

                at += size;
        }

        //      NOERROR, and nothing in it. The name is real and has no address.
        return DNS_NO_ADDRESS;
}

/*
        The servers resolv.conf names, in the order it names them.

        Which servers those are is not decided here -- that is what writes the
        file, and this only reads it. What is decided here is what happens
        when one of them does not answer, and the answer is: ask the next.

        "No such name" does not end the walk either, and that is deliberate
        rather than thorough. The file is written with a public resolver
        first, and a public resolver has never heard of anything inside the
        network it is outside of, so a name that exists only on the local
        network comes back from it as NXDOMAIN. Treating that as final would
        make a machine unable to reach anything on its own network. So it is
        remembered as the answer to fall back on, the rest of the list is
        asked anyway, and only an actual address stops the walk.

        The cost is one extra query for a name that genuinely exists nowhere.

        With no resolv.conf at all there is still somewhere to ask. A machine
        that has not been configured yet should be able to resolve a name, if
        only to fetch the thing that will configure it.
*/
#define DNS_FALLBACK 0x01010101u

static bipolar dns_resolve_any(string_address path, string_address name,
                               p32 address_to found, positive seconds)
{
        bipolar definite = DNS_NO_SERVER;
        bipolar status = DNS_NO_SERVER;
        bipolar server;
        positive index = 0;
        bool asked = false;

        while ((server = dns_server_at(path, index++)) >= 0)
        {
                asked = true;
                status = dns_resolve((p32)server, name, found, seconds);

                if (status == DNS_OK)
                        return DNS_OK;

                if (definite == DNS_NO_SERVER &&
                    (status == DNS_NO_SUCH_NAME || status == DNS_NO_ADDRESS))
                        definite = status;
        }

        if (!asked)
                return dns_resolve(DNS_FALLBACK, name, found, seconds);

        return definite != DNS_NO_SERVER ? definite : status;
}

#endif // STANDARD_MODERN_C_NET_DNS
