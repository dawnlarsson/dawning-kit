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
        positive length;

        address_to port = HTTP_PORT;

        if (!string_compare_max(url, (string_address) "https://", 8))
                return HTTP_NOT_PLAIN;

        if (!string_compare_max(url, (string_address) "http://", 7))
                at = url + 7;

        length = string_span_without_set(at, "/:");
        if (length + 1 >= room)
                return HTTP_BAD_URL;

        memory_copy(host, at, length);
        at += length;
        host[length] = end;

        if (!length)
                return HTTP_BAD_URL;

        if (string_is(at, ':'))
        {
                string_address digits;
                positive bound;
                positive value;

                at++;
                bound = (positive)(string_first_of_or_end(at, '/') - at);
                digits = at;

                if (!bound ||
                    !string_digits_checked(address_of digits, 10,
                                           address_of value) ||
                    (positive)(digits - at) != bound || value > 65535)
                        return HTTP_BAD_URL;

                address_to port = (p16)value;
                at = digits;
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
                                  string_address name, positive address_to length,
                                  bool address_to repeated)
{
        positive at = 0;
        positive want = string_length(name);
        string_address found = null;
        positive found_length = 0;

        if (repeated)
                address_to repeated = false;

        while (at < size)
        {
                positive line = at;
                positive stop = at + memory_span_without_byte(
                    bytes + at, '\n', size - at);

                at = stop + (stop < size);

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

                        if (found)
                        {
                                if (repeated)
                                        address_to repeated = true;
                                continue;
                        }

                        found = (string_address)(bytes + from);
                        found_length = stop - from;
                }
        }

        if (found && length)
                address_to length = found_length;

        return found;
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
                string_address number;
                string_address line_end;
                positive span = memory_span_without_byte(
                    bytes + read, '\n', size - read);

                if (span == size - read)
                        return HTTP_MALFORMED;

                read += span;

                number = (string_address)(bytes + line);
                line_end = (string_address)(bytes + read);
                if (line_end > number && line_end[-1] == '\r')
                        line_end--;

                if (!string_digits_checked(address_of number, 16,
                                           address_of length))
                        return HTTP_MALFORMED;

                //      Chunk extensions do not change framing. Preserve the
                //      semicolon-led extension form while refusing an
                //      arbitrary suffix after the checked length. The tail
                //      is still bounded by the newline found above.
                if (number < line_end && byte_is_blank(number[0]))
                        number += string_span_max(number, line_end - number,
                                                   string_set_blanks);

                if (number < line_end)
                {
                        if (number[0] != ';')
                                return HTTP_MALFORMED;

                        positive extension = line_end - ++number;
                        if (extension && (extension == 1
                                ? escape_categories[number[0]] & 1
                                : memory_escape_index(number, extension, 1) != extension))
                                return HTTP_MALFORMED;
                }

                read++;

                if (!length)
                        return (bipolar)written;

                if (length > size - read)
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

                into = memory_copy_apart_end(into, "GET ", sizeof("GET ") - 1);
                into = memory_copy_apart_end(into, path, path_length);
                into = memory_copy_apart_end(into, " HTTP/1.0\r\nHost: ",
                                             sizeof(" HTTP/1.0\r\nHost: ") - 1);
                into = memory_copy_apart_end(into, name, name_length);
                memory_copy_apart(into,
                    "\r\nUser-Agent: dawning\r\nConnection: close\r\n\r\n",
                    sizeof("\r\nUser-Agent: dawning\r\nConnection: close\r\n\r\n") - 1);

                if (system_write_all((positive)handle, request, used) != used)
                {
                        status = HTTP_NO_REPLY;
                        goto done;
                }
        }

        if (file_store_read((positive)handle, address_of whole) < 0)
        {
                status = HTTP_NO_REPLY;
                goto done;
        }

        socket_close((b32)handle);
        handle = -1;

        if (whole.used < 13)
        {
                status = HTTP_NO_REPLY;
                goto done;
        }

        //      "HTTP/1.x NNN ".  The fixed fields are checked as bytes rather
        //      than accepting a numeric prefix: a corrupt version or status
        //      line is framing, not a successful response with code zero.
        if (string_compare_max(whole.bytes, (string_address) "HTTP/1.", 7) ||
            whole.bytes[7] < '0' || whole.bytes[7] > '9' ||
            whole.bytes[8] != ' ' ||
            whole.bytes[9] < '0' || whole.bytes[9] > '9' ||
            whole.bytes[10] < '0' || whole.bytes[10] > '9' ||
            whole.bytes[11] < '0' || whole.bytes[11] > '9' ||
            (whole.bytes[12] != ' ' && whole.bytes[12] != '\r' &&
             whole.bytes[12] != '\n'))
                goto done;

        if (code)
                address_to code = (b32)((whole.bytes[9] - '0') * 100 +
                                        (whole.bytes[10] - '0') * 10 +
                                        whole.bytes[11] - '0');

        header = http_header_end(whole.bytes, whole.used);

        if (header < 0)
                goto done;

        {
                bool transfer_repeated = false;
                bool length_repeated = false;
                string_address content_length;
                positive content_length_size = 0;

                value = http_header(whole.bytes, (positive)header,
                                    (string_address) "transfer-encoding",
                                    address_of value_length,
                                    address_of transfer_repeated);
                content_length = http_header(
                    whole.bytes, (positive)header,
                    (string_address) "content-length",
                    address_of content_length_size, address_of length_repeated);

                //      More than one framing declaration, or both kinds at
                //      once, is ambiguous. Picking the first lets a proxy and
                //      this client disagree about where the response ends.
                if (transfer_repeated || length_repeated ||
                    (value && content_length))
                        goto done;

                length = whole.used - (positive)header;

                if (value)
                {
                        bipolar plain;

                        // http_header already consumed leading blanks.
                        if (value_length < 7 ||
                            memory_compare_ascii_case(value, "chunked", 7) ||
                            string_span_max(value + 7, value_length - 7,
                                            string_set_blanks) != value_length - 7)
                                goto done;

                        plain = http_unchunk(whole.bytes + header, length);
                        if (plain < 0)
                                goto done;

                        length = (positive)plain;
                }
                else if (content_length)
                {
                        string_address cursor = content_length;
                        positive said;
                        positive at;

                        if (!string_digits_checked(address_of cursor, 10,
                                                   address_of said))
                                goto done;

                        at = (positive)(cursor - content_length);

                        at += string_span_max(cursor, content_length_size - at,
                                              string_set_blanks);

                        //      A short close is not a successful partial
                        //      download, and a numeric prefix is not a valid
                        //      length. Both used to pass silently.
                        if (at != content_length_size || said > length)
                                goto done;

                        length = said;
                }
        }

        //      The complete response already owns enough room for the body.
        //      Compact it in place and hand that allocation to the caller;
        //      reserving a second store doubled peak RAM for every large
        //      download only to release the first one immediately afterward.
        memory_copy(whole.bytes, whole.bytes + header, length);
        whole.used = length;
        whole.bytes[length] = end;

        byte_store_release(body);
        address_to body = whole;
        whole.bytes = null;
        whole.room = 0;
        whole.used = 0;

        status = HTTP_OK;

done:
        if (handle >= 0)
                socket_close((b32)handle);

        byte_store_release(address_of whole);

        return status;
}

#endif // STANDARD_MODERN_C_NET_HTTP
