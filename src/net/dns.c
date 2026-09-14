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
        that does not follow those reads garbage on nearly every reply. Every
        pointer lowers the traversal ceiling, so labels cannot lead back into
        a pointer already followed.

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
#define DNS_NO_RANDOM (-7)

/* Internal result: a validated UDP response asks for the same transaction to
   continue over TCP.  It is never returned to a caller. */
#define DNS_TRY_TCP (-8)

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
        the pointer led. Each jump lowers the bound to its own offset: merely
        moving backwards is insufficient because labels can step forwards to
        that same pointer again.
*/
static PURE bipolar dns_skip_name(p8 address_to message, positive size, positive at)
{
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

                        target = network_load_16(message + at) & 0x3fff;

                        if (target >= at)
                                return DNS_MALFORMED;

                        size = at;
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

/* Expand one wire name into an uncompressed, lower-case label spelling.
   Comparing that spelling keeps owner checks independent of compression and
   of DNS's ASCII case-insensitivity.  The moving ceiling is the same loop
   guard used by dns_skip_name: every compression pointer must move strictly
   backwards and no target may wander back through the pointer that led to it. */
static PURE bipolar dns_copy_name(p8 address_to message, positive size,
                                  positive at, p8 address_to into,
                                  positive room)
{
        positive ceiling = size;
        positive used = 0;

        for (;;)
        {
                p8 length;

                if (at >= ceiling)
                        return DNS_MALFORMED;

                length = message[at];

                if ((length & 0xc0) == 0xc0)
                {
                        positive target;

                        if (at + 1 >= ceiling)
                                return DNS_MALFORMED;

                        target = network_load_16(message + at) & 0x3fff;

                        if (target >= at)
                                return DNS_MALFORMED;

                        ceiling = at;
                        at = target;
                        continue;
                }

                if (length & 0xc0 || length > 63 ||
                    length > ceiling - at - 1 ||
                    used > room || room - used < (positive)length + 1)
                        return DNS_MALFORMED;

                into[used++] = length;
                at++;

                for (positive byte = 0; byte < length; byte++)
                {
                        p8 value = message[at + byte];

                        if (value >= 'A' && value <= 'Z')
                                value = (p8)(value + ('a' - 'A'));
                        into[used++] = value;
                }

                at += length;

                if (!length)
                        return (bipolar)used;
        }
}

/* Find an address only along the name that was asked for and the CNAME chain
   rooted at it.  An answer packet may legally put the terminal A before its
   CNAME, so each bounded pass considers one link rather than trusting record
   order.  Unrelated A records are glue or attacker-controlled distractions,
   never an answer to the question. */
static bipolar dns_answer_address(p8 address_to message, positive size,
                                  positive records_at, p16 answers,
                                  positive question_at,
                                  p32 address_to found)
{
        p8 wanted[256];
        p8 alias[256];
        bipolar wanted_length = dns_copy_name(message, size, question_at,
                                              wanted, sizeof wanted);

        if (wanted_length < 0)
                return DNS_MALFORMED;

        /* A cycle needs no more links than there are answer records to show
           itself.  The extra pass is the one that can find the terminal A. */
        for (positive hop = 0; hop <= (positive)answers; hop++)
        {
                positive at = records_at;
                bool has_alias = false;
                bool has_address = false;
                bipolar alias_length = 0;
                p32 address = 0;

                for (positive record = 0; record < answers; record++)
                {
                        p8 owner[256];
                        bipolar owner_length = dns_copy_name(
                            message, size, at, owner, sizeof owner);
                        bipolar next = dns_skip_name(message, size, at);
                        p16 kind;
                        p16 class;
                        p16 data_length;
                        bool is_wanted;

                        if (owner_length < 0 || next < 0)
                                return DNS_MALFORMED;

                        at = (positive)next;

                        if (at > size || size - at < 10)
                                return DNS_MALFORMED;

                        kind = network_load_16(message + at);
                        class = network_load_16(message + at + 2);
                        data_length = network_load_16(message + at + 8);
                        at += 10;

                        if (data_length > size - at)
                                return DNS_MALFORMED;

                        is_wanted = owner_length == wanted_length &&
                                    !memory_compare(owner, wanted,
                                                    (positive)wanted_length);

                        if (class == DNS_CLASS_IN && is_wanted &&
                            kind == DNS_TYPE_A)
                        {
                                if (data_length != 4)
                                        return DNS_MALFORMED;
                                if (!has_address)
                                        address = network_load_32(message + at);
                                has_address = true;
                        }
                        else if (class == DNS_CLASS_IN && is_wanted &&
                                 kind == DNS_TYPE_CNAME)
                        {
                                bipolar target_end;

                                if (has_alias)
                                        return DNS_MALFORMED;

                                target_end = dns_skip_name(message, size, at);
                                alias_length = dns_copy_name(
                                    message, size, at, alias, sizeof alias);

                                if (target_end < 0 || alias_length < 0 ||
                                    (positive)target_end != at + data_length)
                                        return DNS_MALFORMED;
                                has_alias = true;
                        }

                        at += data_length;
                }

                /* CNAME and other data at one owner are mutually exclusive.
                   Treating a packet containing both as an address choice
                   would make its meaning depend on record order. */
                if (has_alias && has_address)
                        return DNS_MALFORMED;

                if (has_address)
                {
                        if (found)
                                *found = address;
                        return DNS_OK;
                }

                if (!has_alias)
                        return DNS_NO_ADDRESS;

                if (alias_length == wanted_length &&
                    !memory_compare(alias, wanted,
                                    (positive)wanted_length))
                        return DNS_MALFORMED;

                memory_copy(wanted, alias, (positive)alias_length);
                wanted_length = alias_length;
        }

        return DNS_MALFORMED;
}

/* The transaction id and exact echoed question are the reply identity.  UDP
   uses this predicate to discard raced junk within the original deadline;
   TCP has one framed reply and treats an identity mismatch as malformed. */
static bool dns_reply_identity(
    p8 address_to reply, positive size, p16 id,
    p8 address_to request, positive question_length)
{
        return size >= DNS_HEADER && network_load_16(reply) == id &&
               network_load_16(reply + 4) == 1 &&
               size >= DNS_HEADER + question_length &&
               !memory_compare(reply + DNS_HEADER,
                               request + DNS_HEADER, question_length);
}

/* UDP and TCP answers pass through the same response, rcode and record
   validation.  Only a validated truncation indication has a distinct internal
   result so the transport can retry it over TCP. */
static bipolar dns_reply_result(
    p8 address_to reply, positive size, p16 id,
    p8 address_to request, positive question_length,
    p32 address_to found)
{
        p16 flags;
        p16 answers;
        positive at;

        positive available = size > DNS_MAX_MESSAGE ? DNS_MAX_MESSAGE : size;

        if (!dns_reply_identity(reply, available, id, request,
                                question_length))
                return DNS_MALFORMED;

        flags = network_load_16(reply + 2);
        if (!(flags & DNS_FLAG_RESPONSE))
                return DNS_MALFORMED;
        if (flags & DNS_FLAG_TRUNCATED)
                return DNS_TRY_TCP;
        /* MSG_TRUNC reports the datagram's true length.  A matching TC reply
           needs only its complete header and question to authorize TCP; any
           oversized response that claims to be complete remains malformed. */
        if (size > DNS_MAX_MESSAGE)
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
        return dns_answer_address(reply, size, at, answers, DNS_HEADER, found);
}

static bipolar dns_stream_connect_until(
    socket_address_internet address_to where,
    const network_deadline address_to deadline)
{
        bipolar handle = socket_new(
            AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        bipolar status;

        if (handle < 0)
                return -1;

        status = socket_connect((b32)handle, where, sizeof *where);
        if (status < 0 && status != -EINPROGRESS && status != -EALREADY &&
            status != NETWORK_INTERRUPTED)
                goto failed;

        if (status < 0)
        {
                b32 error = 0;
                p32 error_size = sizeof error;

                if (network_wait_writable_until(handle, deadline) <= 0 ||
                    socket_option_get((b32)handle, SOL_SOCKET, SO_ERROR,
                                      address_of error, address_of error_size) < 0 ||
                    error_size != sizeof error || error)
                        goto failed;
        }

        return handle;

failed:
        socket_close((b32)handle);
        return -1;
}

static bipolar dns_retry_tcp(
    socket_address_internet address_to where,
    p8 address_to request, positive request_length, p16 id,
    positive question_length, p32 address_to found,
    const network_deadline address_to deadline)
{
        p8 reply[DNS_MAX_MESSAGE];
        p8 frame[2];
        bipolar handle = dns_stream_connect_until(where, deadline);
        p16 length;
        bipolar result = DNS_NO_REPLY;

        if (handle < 0)
                return DNS_NO_REPLY;

        network_store_16(frame, (p16)request_length);
        if (!network_stream_send_all_until(handle, frame, sizeof frame,
                                           deadline) ||
            !network_stream_send_all_until(handle, request, request_length,
                                           deadline) ||
            !network_stream_read_all(handle, frame, sizeof frame, deadline))
                goto done;

        length = network_load_16(frame);
        if (length > sizeof reply)
        {
                result = DNS_MALFORMED;
                goto done;
        }
        if (!network_stream_read_all(handle, reply, length, deadline))
                goto done;

        result = dns_reply_result(reply, length, id, request,
                                  question_length, found);
        if (result == DNS_TRY_TCP)
                result = DNS_MALFORMED;

done:
        socket_close((b32)handle);
        return result;
}

/*
        The nameserver, out of resolv.conf.

        Only "nameserver A.B.C.D" lines: the keyword, blanks, and the address
        up to the next blank, so a tab or a trailing comment does not hide a
        server. The wanted-th of them is returned, so a caller walks 0, 1, 2
        until this answers negatively and the number of servers a machine may
        list has no ceiling. Options, search domains and IPv6 servers are read
        past rather than understood.
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
                positive stop = at + memory_span_without_byte(
                    text + at, '\n', (positive)got - at);
                positive from = line + 10;
                positive length = 0;
                p8 kept[64];
                bipolar host;

                at = stop + (stop < (positive)got);

                if (stop - line < 12 ||
                    memory_compare(text + line, "nameserver", 10) ||
                    !byte_is_blank(text[from]))
                        continue;

                from += string_span_max(text + from, stop - from,
                                        string_set_blanks);
                while (from + length < stop && text[from + length] != '\r' &&
                       !byte_is_blank(text[from + length]))
                        length++;
                if (!length || length >= sizeof kept)
                        continue;

                string_copy_max_end(kept, text + from, length);
                host = string_to_host(kept);
                if (host >= 0 && !wanted--)
                        return host;
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
static bipolar dns_resolve_at(p32 server, p16 port, string_address name,
                              p32 address_to found, positive seconds)
{
        p8 request[DNS_MAX_MESSAGE];
        p8 reply[DNS_MAX_MESSAGE];
        p16 id;
        bipolar handle;
        bipolar written;
        bipolar got;
        bipolar failure = DNS_NO_REPLY;
        positive question_length;
        network_deadline deadline;

        if (!network_transaction_secure(address_of id, sizeof id))
                return DNS_NO_RANDOM;

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

        /* TCP fallback spends only what the original UDP transaction leaves.
           Start the one monotonic budget before any socket operation. */
        if (!network_deadline_begin(address_of deadline, seconds, 0))
                return DNS_NO_REPLY;

        handle = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

        if (handle < 0)
                return DNS_NO_SERVER;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(port),
            .host = network_order_32(server)};

        if (socket_connect((b32)handle, address_of where, sizeof where) < 0)
        {
                failure = DNS_NO_SERVER;
                goto failed;
        }

        if (socket_send((b32)handle, request, DNS_HEADER + question_length,
                        0, 0, 0) < 0)
                goto failed;

        /* A connected UDP socket authenticates the source address, not the
           transaction. An off-path sender can still spoof that address and
           race junk without guessing the random id. Discard replies that do
           not match both id and question, while charging every one to the
           original total deadline. */
        for (;;)
        {
                positive available;

                got = network_wait_readable_until(handle, address_of deadline);

                if (got <= 0)
                        goto failed;

                got = socket_receive((b32)handle, reply, sizeof reply,
                                     MSG_TRUNC, 0, 0);

                if (got == NETWORK_INTERRUPTED)
                        continue;
                if (got < 0)
                {
                        /* ICMP port unreachable arrives as ECONNREFUSED on
                           the connected socket: nobody will answer here. */
                        failure = DNS_NO_SERVER;
                        goto failed;
                }
                available = (positive)got > sizeof reply
                    ? sizeof reply : (positive)got;
                if (!dns_reply_identity(reply, available, id, request,
                                        question_length))
                        continue;

                break;
        }

        socket_close((b32)handle);

        failure = dns_reply_result(reply, (positive)got, id, request,
                                   question_length, found);
        if (failure == DNS_TRY_TCP)
                return dns_retry_tcp(address_of where, request,
                                     DNS_HEADER + question_length, id,
                                     question_length, found,
                                     address_of deadline);
        return failure;

failed:
        socket_close((b32)handle);
        return failure;
}

static bipolar dns_resolve(p32 server, string_address name, p32 address_to found,
                           positive seconds)
{
        return dns_resolve_at(server, DNS_PORT, name, found, seconds);
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
