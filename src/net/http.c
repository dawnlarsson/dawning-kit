/*
        Experimental C standard library

        http: a URL, and the bytes behind it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_HTTP
#define STANDARD_MODERN_C_NET_HTTP

/*
        Enough HTTP to fetch a file, and no more.

        http:// only. Nobody surveyed TLS and nothing here does it: https is a
        certificate store, a handshake, and a cipher implementation, which is
        months rather than an afternoon, and pretending otherwise by quietly
        fetching over plaintext would be worse than saying so. A URL that
        begins https:// is refused by name.

        Two ways a body ends and both are needed. Content-Length is the easy
        one. Chunked is the one a minimal client is tempted to skip, and it is
        what most servers actually send when the length is not known in
        advance, so skipping it means the common case arrives as gibberish
        with its framing still in it.

        The response is read into a buffer that grows, because the length is
        not known until the header says so and may not be said at all.
*/

#define HTTP_PORT 80

#define HTTP_OK 0
#define HTTP_BAD_URL (-1)
#define HTTP_NO_HOST (-2)
#define HTTP_NO_ROUTE (-3)
#define HTTP_NO_REPLY (-4)
#define HTTP_MALFORMED (-5)
#define HTTP_NOT_PLAIN (-6)

typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
} http_buffer;

static bool http_room(http_buffer address_to buffer, positive want)
{
        return memory_reserve((address_any address_to)address_of buffer->bytes,
                              address_of buffer->room, buffer->used, want, 1, 65536);
}

static fn http_forget(http_buffer address_to buffer)
{
        memory_release((address_any address_to)address_of buffer->bytes,
                       address_of buffer->room, address_of buffer->used, 1);
}

/*
        http://host[:port][/path] taken apart.

        Everything before the first slash after the authority is the host,
        everything from it is the path, and a missing path is "/". A colon in
        the authority is a port, which is how a test talks to a server on a
        port the kernel picked.
*/
static bipolar http_split(string_address url, p8 address_to host, positive room,
                          p16 address_to port, string_address address_to path)
{
        string_address at = url;
        positive length = 0;

        address_to port = HTTP_PORT;

        if (!string_compare_max(url, (string_address) "https://", 8))
                return HTTP_NOT_PLAIN;

        if (!string_compare_max(url, (string_address) "http://", 7))
                at = url + 7;

        while (string_get(at) && !string_is(at, '/') && !string_is(at, ':'))
        {
                if (length + 1 >= room)
                        return HTTP_BAD_URL;

                host[length++] = string_get(at);
                at++;
        }

        host[length] = end;

        if (!length)
                return HTTP_BAD_URL;

        if (string_is(at, ':'))
        {
                positive value = 0;

                at++;

                while (string_get(at) && !string_is(at, '/'))
                {
                        if (string_get(at) < '0' || string_get(at) > '9')
                                return HTTP_BAD_URL;

                        value = value * 10 + (positive)(string_get(at) - '0');

                        if (value > 65535)
                                return HTTP_BAD_URL;

                        at++;
                }

                address_to port = (p16)value;
        }

        address_to path = string_get(at) ? at : (string_address) "/";

        return HTTP_OK;
}

//      Where the header ends: the blank line, in either spelling.
static bipolar http_header_end(p8 address_to bytes, positive size)
{
        positive at = 0;

        while (at + 1 < size)
        {
                if (bytes[at] == '\n' && bytes[at + 1] == '\n')
                        return (bipolar)(at + 2);

                if (at + 3 < size && bytes[at] == '\r' && bytes[at + 1] == '\n' &&
                    bytes[at + 2] == '\r' && bytes[at + 3] == '\n')
                        return (bipolar)(at + 4);

                at++;
        }

        return -1;
}

//      One header's value, by name, without regard to its case.
static string_address http_header(p8 address_to bytes, positive size,
                                  string_address name, positive address_to length)
{
        positive at = 0;
        positive want = string_length(name);

        while (at < size)
        {
                positive line = at;
                p8 address_to newline = (p8 address_to)memory_first_of(
                    bytes + at, '\n', size - at);
                positive stop = newline ? (positive)(newline - bytes) : size;

                at = newline ? stop + 1 : stop;

                if (stop > line && bytes[stop - 1] == '\r')
                        stop--;

                if (stop - line <= want || bytes[line + want] != ':')
                        continue;

                {
                        positive index = 0;
                        bool same = true;

                        while (index < want)
                        {
                                p8 one = bytes[line + index];
                                p8 two = (p8)name[index];

                                if (one >= 'A' && one <= 'Z')
                                        one = (p8)(one + 32);

                                if (two >= 'A' && two <= 'Z')
                                        two = (p8)(two + 32);

                                if (one != two)
                                {
                                        same = false;
                                        break;
                                }

                                index++;
                        }

                        if (!same)
                                continue;
                }

                {
                        positive from = line + want + 1;

                        from += string_span_max((string_address)(bytes + from), stop - from,
                                                string_set_blanks);

                        if (length)
                                address_to length = stop - from;

                        return (string_address)(bytes + from);
                }
        }

        return null;
}

//      Appending, with the room checked once per piece rather than never.
static positive http_add(p8 address_to into, positive used, positive room,
                         string_address text)
{
        positive length = string_length(text);

        if (used + length + 1 > room)
                return used;

        memory_copy(into + used, text, length);

        return used + length;
}

/*
        Chunked, unwrapped in place.

        Each chunk is a hexadecimal length on its own line, that many bytes,
        then a blank line, ending with a zero length. Unwrapping in place is
        safe because what is written is always behind what is read.
*/
static bipolar http_unchunk(p8 address_to bytes, positive size)
{
        positive read = 0;
        positive written = 0;

        for (;;)
        {
                positive line = read;
                positive length;
                p8 address_to newline = (p8 address_to)memory_first_of(
                    bytes + read, '\n', size - read);

                if (!newline)
                        return HTTP_MALFORMED;

                read = (positive)(newline - bytes);

                length = string_digits_hexadecimal_max(
                    (string_address)(bytes + line), read - line, null);
                read++;

                if (!length)
                        return (bipolar)written;

                if (read + length > size)
                        return HTTP_MALFORMED;

                memory_copy(bytes + written, bytes + read, length);
                written += length;
                read += length;

                while (read < size && (bytes[read] == '\r' || bytes[read] == '\n'))
                        read++;
        }
}

/*
        The whole exchange.

        The caller is handed the body and the status line's code. Redirects
        are reported rather than followed: a client that follows them needs a
        loop limit, a same-host rule and an opinion about relative locations,
        and none of that belongs in the first version.
*/
static bipolar http_get(p32 host, p16 port, string_address name,
                        string_address path, http_buffer address_to body,
                        b32 address_to code)
{
        socket_address_internet where;
        http_buffer whole = {0};
        bipolar handle;
        bipolar got;
        bipolar header;
        positive length = 0;
        string_address value;
        positive value_length = 0;

        handle = socket_new(AF_INET, SOCK_STREAM, 0);

        if (handle < 0)
                return HTTP_NO_ROUTE;

        memory_fill(address_of where, 0, sizeof where);
        where.family = AF_INET;
        where.port = network_order_16(port);
        where.host = network_order_32(host);

        if (socket_connect((b32)handle, address_of where, sizeof where) < 0)
        {
                socket_close((b32)handle);
                return HTTP_NO_ROUTE;
        }

        //      HTTP/1.0 with an explicit close, so the server ends the body by
        //      ending the connection and there is no keep-alive to unwind.
        //      Host: is sent anyway, because a name-based server needs it and
        //      answers 400 without it whatever the version says.
        if (!http_room(address_of whole, 65536))
        {
                socket_close((b32)handle);
                return HTTP_MALFORMED;
        }

        {
                p8 request[1024];
                positive used = 0;

                used = http_add(request, used, sizeof request, (string_address) "GET ");
                used = http_add(request, used, sizeof request, path);
                used = http_add(request, used, sizeof request,
                                (string_address) " HTTP/1.0\r\nHost: ");
                used = http_add(request, used, sizeof request, name);
                used = http_add(request, used, sizeof request,
                                (string_address) "\r\nUser-Agent: dawning\r\n"
                                                 "Connection: close\r\n\r\n");

                if (socket_send((b32)handle, request, used, 0, 0, 0) < 0)
                {
                        socket_close((b32)handle);
                        http_forget(address_of whole);
                        return HTTP_NO_REPLY;
                }
        }

        for (;;)
        {
                if (!http_room(address_of whole, whole.used + 65536))
                {
                        socket_close((b32)handle);
                        http_forget(address_of whole);
                        return HTTP_MALFORMED;
                }

                got = socket_receive((b32)handle, whole.bytes + whole.used,
                                     whole.room - whole.used - 1, 0, 0, 0);

                if (got <= 0)
                        break;

                whole.used += (positive)got;
        }

        socket_close((b32)handle);

        if (whole.used < 12)
        {
                http_forget(address_of whole);
                return HTTP_NO_REPLY;
        }

        //      "HTTP/1.x NNN "
        if (string_compare_max(whole.bytes, (string_address) "HTTP/1.", 7))
        {
                http_forget(address_of whole);
                return HTTP_MALFORMED;
        }

        if (code)
                address_to code = (b32)string_digits_max(
                    (string_address)(whole.bytes + 9), 3, null);

        header = http_header_end(whole.bytes, whole.used);

        if (header < 0)
        {
                http_forget(address_of whole);
                return HTTP_MALFORMED;
        }

        value = http_header(whole.bytes, (positive)header,
                            (string_address) "transfer-encoding", address_of value_length);

        length = whole.used - (positive)header;

        if (value && value_length >= 7 &&
            !string_compare_max(value, (string_address) "chunked", 7))
        {
                bipolar plain = http_unchunk(whole.bytes + header, length);

                if (plain < 0)
                {
                        http_forget(address_of whole);
                        return HTTP_MALFORMED;
                }

                length = (positive)plain;
        }
        else
        {
                value = http_header(whole.bytes, (positive)header,
                                    (string_address) "content-length",
                                    address_of value_length);

                if (value)
                {
                        positive said = string_digits_max(value, value_length, null);

                        if (said < length)
                                length = said;
                }
        }

        if (!http_room(body, length + 1))
        {
                http_forget(address_of whole);
                return HTTP_MALFORMED;
        }

        memory_copy(body->bytes, whole.bytes + header, length);
        body->used = length;
        body->bytes[length] = end;

        http_forget(address_of whole);

        return HTTP_OK;
}

#endif // STANDARD_MODERN_C_NET_HTTP
