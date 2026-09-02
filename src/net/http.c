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

typedef byte_store http_buffer;
#define http_forget(buffer) byte_store_release(buffer)

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

        //      The authority ends at the first slash or colon. One walk is
        //      enough; asking for each byte separately scanned every normal
        //      hostname twice.
        {
                string_address stop = at;

                while (string_get(stop) && !string_is(stop, '/') &&
                       !string_is(stop, ':'))
                        stop++;

                length = (positive)(stop - at);

                if (length + 1 >= room)
                        return HTTP_BAD_URL;

                memory_copy(host, at, length);
                at = stop;
        }

        host[length] = end;

        if (!length)
                return HTTP_BAD_URL;

        if (string_is(at, ':'))
        {
                positive used;
                positive bound;
                positive value;

                at++;
                bound = (positive)(string_first_of_or_end(at, '/') - at);
                value = string_digits_max(at, bound, address_of used);

                if (!bound || used != bound || value > 65535)
                        return HTTP_BAD_URL;

                address_to port = (p16)value;
                at += used;
        }

        address_to path = string_get(at) ? at : (string_address) "/";

        return HTTP_OK;
}

//      Where the header ends: the blank line, in either spelling.
static PURE bipolar http_header_end(p8 address_to bytes, positive size)
{
        //      Both spellings are looked for at once and the one that
        //      starts first wins. A header block ending \r\n\r\n contains no
        //      bare \n\n -- there is a \r between them -- so the two can never
        //      both match at the same place.
        p8 address_to full = (p8 address_to)memory_search(bytes, size, "\r\n\r\n", 4);
        p8 address_to bare = (p8 address_to)memory_search(bytes, size, "\n\n", 2);

        if (full && (!bare || full < bare))
                return (bipolar)(full - bytes) + 4;

        if (bare)
                return (bipolar)(bare - bytes) + 2;

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

                if (memory_compare_ascii_case(bytes + line, name, want))
                        continue;

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
                positive used;
                p8 address_to newline = (p8 address_to)memory_first_of(
                    bytes + read, '\n', size - read);

                if (!newline)
                        return HTTP_MALFORMED;

                read = (positive)(newline - bytes);

                length = string_digits_hexadecimal_max(
                    (string_address)(bytes + line), read - line, address_of used);
                read++;

                if (!used)
                        return HTTP_MALFORMED;

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
        http_buffer whole = {0};
        bipolar handle;
        bipolar got;
        bipolar header;
        bipolar status = HTTP_MALFORMED;
        positive length = 0;
        string_address value;
        positive value_length = 0;

        handle = socket_new(AF_INET, SOCK_STREAM, 0);

        if (handle < 0)
                return HTTP_NO_ROUTE;

        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(port),
            .host = network_order_32(host)};

        if (socket_connect((b32)handle, address_of where, sizeof where) < 0)
        {
                status = HTTP_NO_ROUTE;
                goto done;
        }

        //      HTTP/1.0 with an explicit close, so the server ends the body by
        //      ending the connection and there is no keep-alive to unwind.
        //      Host: is sent anyway, because a name-based server needs it and
        //      answers 400 without it whatever the version says.
        if (!byte_store_reserve(address_of whole, 65536, 65536))
                goto done;

        {
                p8 request[1024];
                positive path_length = string_length(path);
                positive name_length = string_length(name);
                positive fixed = sizeof("GET ") - 1 +
                                 sizeof(" HTTP/1.0\r\nHost: ") - 1 +
                                 sizeof("\r\nUser-Agent: dawning\r\n"
                                        "Connection: close\r\n\r\n") - 1;
                positive used;
                p8 address_to into = request;

                if (path_length > sizeof request - fixed ||
                    name_length > sizeof request - fixed - path_length)
                {
                        status = HTTP_BAD_URL;
                        goto done;
                }

                used = fixed + path_length + name_length;

#define HTTP_COPY(part, length)                                              \
                do { memory_copy(into, (part), (length)); into += (length); } \
                while (false)
                HTTP_COPY("GET ", sizeof("GET ") - 1);
                HTTP_COPY(path, path_length);
                HTTP_COPY(" HTTP/1.0\r\nHost: ",
                          sizeof(" HTTP/1.0\r\nHost: ") - 1);
                HTTP_COPY(name, name_length);
                HTTP_COPY("\r\nUser-Agent: dawning\r\nConnection: close\r\n\r\n",
                          sizeof("\r\nUser-Agent: dawning\r\n"
                                 "Connection: close\r\n\r\n") - 1);
#undef HTTP_COPY

                if (socket_send((b32)handle, request, used, 0, 0, 0) < 0)
                {
                        status = HTTP_NO_REPLY;
                        goto done;
                }
        }

        for (;;)
        {
                if (!byte_store_reserve(address_of whole,
                                        whole.used + 65536, 65536))
                        goto done;

                got = socket_receive((b32)handle, whole.bytes + whole.used,
                                     whole.room - whole.used - 1, 0, 0, 0);

                if (got <= 0)
                        break;

                whole.used += (positive)got;
        }

        socket_close((b32)handle);
        handle = -1;

        if (whole.used < 12)
        {
                status = HTTP_NO_REPLY;
                goto done;
        }

        //      "HTTP/1.x NNN "
        if (string_compare_max(whole.bytes, (string_address) "HTTP/1.", 7))
                goto done;

        if (code)
                address_to code = (b32)string_digits_max(
                    (string_address)(whole.bytes + 9), 3, null);

        header = http_header_end(whole.bytes, whole.used);

        if (header < 0)
                goto done;

        value = http_header(whole.bytes, (positive)header,
                            (string_address) "transfer-encoding", address_of value_length);

        length = whole.used - (positive)header;

        if (value && value_length >= 7 &&
            !string_compare_max(value, (string_address) "chunked", 7))
        {
                bipolar plain = http_unchunk(whole.bytes + header, length);

                if (plain < 0)
                        goto done;

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

        if (!byte_store_reserve(body, length + 1, 65536))
                goto done;

        memory_copy(body->bytes, whole.bytes + header, length);
        body->used = length;
        body->bytes[length] = end;

        status = HTTP_OK;

done:
        if (handle >= 0)
                socket_close((b32)handle);

        byte_store_release(address_of whole);

        return status;
}

#endif // STANDARD_MODERN_C_NET_HTTP
