/*
        Experimental C standard library

        http: a URL, and the bytes behind it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_HTTP
#define STANDARD_MODERN_C_NET_HTTP

#include "tls.c"

/*
        Enough HTTP to fetch a file, and no more.

        wget speaks https the way BusyBox does: one GET, redirects, and the
        body streamed to a file. fetch stays the small plaintext tool; it
        still slurps. A body ends three ways. Content-Length is the easy one.
        Chunked is what most servers send when the length is not known in
        advance. Close-delimited is the rest: TLS close_notify ends an
        HTTPS body with no length; a plaintext peer that never closes still
        waits.
*/

#define HTTP_PORT 80
#define HTTP_HTTPS_PORT 443
#define HTTP_URL_MAX 2048
#define HTTP_HEAD_MAX 16384
#define HTTP_HOPS 10

#define HTTP_OK 0
#define HTTP_BAD_URL (-1)
#define HTTP_NO_HOST (-2)
#define HTTP_NO_ROUTE (-3)
#define HTTP_NO_REPLY (-4)
#define HTTP_MALFORMED (-5)
#define HTTP_NOT_PLAIN (-6)
#define HTTP_TLS (-7)
#define HTTP_REDIRECTS (-8)

typedef byte_store http_buffer;
#define http_forget(buffer) byte_store_release(buffer)

/*
        http://host[:port][/path] taken apart.

        Everything before the first slash after the authority is the host,
        everything from it is the path, and a missing path is "/". A colon in
        the authority is a port, which is how a test talks to a server on a
        port the kernel picked.
*/
static bipolar http_split_into(string_address url, p8 address_to host, positive room,
                               p16 address_to port, string_address address_to path,
                               bool address_to tls)
{
        string_address at = url;
        positive length;

        address_to tls = false;
        address_to port = HTTP_PORT;

        for (string_address scan = url; *scan; scan++)
                if (byte_is_control(*scan) || *scan == ' ')
                        return HTTP_BAD_URL;

        if (!string_compare_max(url, (string_address) "https://", 8))
        {
                address_to tls = true;
                address_to port = HTTP_HTTPS_PORT;
                at = url + 8;
        }
        else if (!string_compare_max(url, (string_address) "http://", 7))
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

static bipolar http_split(string_address url, p8 address_to host, positive room,
                          p16 address_to port, string_address address_to path)
{
        bool tls = false;

        return http_split_into(url, host, room, port, path, address_of tls);
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

                if (read < size && bytes[read] == '\r')
                        read++;
                if (read >= size || bytes[read++] != '\n')
                        return HTTP_MALFORMED;
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

typedef struct
{
        bipolar handle;
        bool tls;
        tls_conn session;
} http_link;

typedef struct
{
        http_link address_to link;
        p8 address_to stash;
        positive stash_used;
} http_body;

static fn http_link_close(http_link address_to link)
{
        if (link->handle >= 0)
                socket_close((b32)link->handle);
        link->handle = -1;
}

static bipolar http_link_open(http_link address_to link, p32 ip, p16 port,
                              string_address host, bool tls, bool check_cert)
{
        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(port),
            .host = network_order_32(ip)};

        memory_fill(link, 0, sizeof(*link));
        link->handle = socket_new(AF_INET, SOCK_STREAM, 0);
        if (link->handle < 0)
                return HTTP_NO_ROUTE;
        if (socket_connect((b32)link->handle, address_of where, sizeof where) < 0)
        {
                http_link_close(link);
                return HTTP_NO_ROUTE;
        }
        if (tls)
        {
                if (tls_connect(address_of link->session, link->handle, host,
                                check_cert))
                {
                        http_link_close(link);
                        return HTTP_TLS;
                }
                link->tls = true;
        }
        return HTTP_OK;
}

static bipolar http_link_write(http_link address_to link, p8 address_to data,
                               positive length)
{
        if (link->tls)
                return tls_write(address_of link->session, data, length) ? HTTP_NO_REPLY
                                                                         : HTTP_OK;
        if (system_write_all((positive)link->handle, data, length) != length)
                return HTTP_NO_REPLY;
        return HTTP_OK;
}

static bipolar http_link_read(http_link address_to link, p8 address_to into,
                              positive room, positive address_to got)
{
        bipolar n;

        if (link->tls)
        {
                if (tls_read(address_of link->session, into, room, got))
                        return HTTP_NO_REPLY;
                return HTTP_OK;
        }

        n = system_read_retry((positive)link->handle, into, room);
        if (n < 0)
                return HTTP_NO_REPLY;
        address_to got = (positive)n;
        return HTTP_OK;
}

static bipolar http_body_read(http_body address_to body, p8 address_to into,
                              positive room, positive address_to got)
{
        if (body->stash_used)
        {
                positive take = body->stash_used;
                if (take > room)
                        take = room;
                memory_copy(into, body->stash, take);
                memory_copy(body->stash, body->stash + take, body->stash_used - take);
                body->stash_used -= take;
                address_to got = take;
                return HTTP_OK;
        }

        return http_link_read(body->link, into, room, got);
}

static bipolar http_copy_n(http_body address_to body, bipolar dest, positive want)
{
        p8 bounce[8192];

        while (want)
        {
                positive take = want;
                positive got = 0;

                if (take > sizeof bounce)
                        take = sizeof bounce;
                if (http_body_read(body, bounce, take, address_of got))
                        return HTTP_NO_REPLY;
                if (!got)
                        return HTTP_NO_REPLY;
                if (system_write_all((positive)dest, bounce, got) != got)
                        return HTTP_NO_REPLY;
                want -= got;
        }

        return HTTP_OK;
}

static bipolar http_copy_all(http_body address_to body, bipolar dest)
{
        p8 bounce[8192];

        for (;;)
        {
                positive got = 0;

                if (http_body_read(body, bounce, sizeof bounce, address_of got))
                        return HTTP_NO_REPLY;
                if (!got)
                        return HTTP_OK;
                if (system_write_all((positive)dest, bounce, got) != got)
                        return HTTP_NO_REPLY;
        }
}

static bipolar http_line(http_body address_to body, p8 address_to into, positive room,
                         positive address_to length)
{
        positive used = 0;

        while (used + 1 < room)
        {
                positive got = 0;
                p8 byte;

                if (http_body_read(body, address_of byte, 1, address_of got))
                        return HTTP_NO_REPLY;
                if (!got)
                        return HTTP_MALFORMED;
                into[used++] = byte;
                if (byte == '\n')
                {
                        address_to length = used;
                        into[used] = end;
                        return HTTP_OK;
                }
        }

        return HTTP_MALFORMED;
}

static bipolar http_body_fill(http_body address_to body, p8 address_to into,
                              positive want)
{
        positive have = 0;

        while (have < want)
        {
                positive got = 0;

                if (http_body_read(body, into + have, want - have, address_of got) ||
                    !got)
                        return HTTP_NO_REPLY;
                have += got;
        }

        return HTTP_OK;
}

static bipolar http_copy_chunked(http_body address_to body, bipolar dest)
{
        p8 line[128];

        for (;;)
        {
                positive line_length = 0;
                positive size = 0;
                string_address number;
                string_address stop;
                p8 crlf[2];

                if (http_line(body, line, sizeof line, address_of line_length))
                        return HTTP_MALFORMED;
                number = (string_address)line;
                stop = (string_address)(line + line_length);
                if (stop > number && stop[-1] == '\n')
                        stop--;
                if (stop > number && stop[-1] == '\r')
                        stop--;
                if (!string_digits_checked(address_of number, 16, address_of size))
                        return HTTP_MALFORMED;
                if (number < stop && byte_is_blank(number[0]))
                        number += string_span_max(number, stop - number,
                                                  string_set_blanks);
                if (number < stop && number[0] != ';')
                        return HTTP_MALFORMED;
                if (!size)
                        return HTTP_OK;
                if (http_copy_n(body, dest, size))
                        return HTTP_NO_REPLY;
                if (http_body_fill(body, crlf, 2))
                        return HTTP_MALFORMED;
                if (crlf[0] == '\r' && crlf[1] == '\n')
                        continue;
                if (crlf[0] == '\n')
                {
                        if (body->stash_used)
                                return HTTP_MALFORMED;
                        body->stash[0] = crlf[1];
                        body->stash_used = 1;
                        continue;
                }
                return HTTP_MALFORMED;
        }
}

static positive http_digits(p8 address_to into, positive value)
{
        p8 tmp[10];
        positive n = 0;
        positive i;

        if (!value)
        {
                into[0] = '0';
                return 1;
        }

        while (value)
        {
                tmp[n++] = (p8)('0' + (value % 10));
                value /= 10;
        }
        for (i = 0; i < n; i++)
                into[i] = tmp[n - 1 - i];
        return n;
}

static bipolar http_put_url(p8 address_to into, positive room, bool tls,
                            string_address host, p16 port, string_address path)
{
        p8 address_to at = into;
        positive host_length = string_length(host);
        positive path_length;
        positive scheme_length = tls ? 8 : 7;
        bool named_port = (tls && port != HTTP_HTTPS_PORT) ||
                          (!tls && port != HTTP_PORT);
        p8 port_text[6];
        positive port_length = 0;

        if (!string_get(path))
                path = (string_address) "/";
        path_length = string_length(path);
        if (named_port)
                port_length = 1 + http_digits(port_text, port);
        if (scheme_length + host_length + port_length + path_length + 1 > room)
                return HTTP_BAD_URL;

        at = memory_copy_apart_end(at, tls ? "https://" : "http://", scheme_length);
        at = memory_copy_apart_end(at, host, host_length);
        if (named_port)
        {
                at[0] = ':';
                at++;
                at = memory_copy_apart_end(at, port_text, port_length - 1);
        }
        at = memory_copy_apart_end(at, path, path_length);
        at[0] = end;
        return HTTP_OK;
}

static bipolar http_absolutize(bool tls, string_address host, p16 port,
                               string_address path, string_address location,
                               p8 address_to into, positive room)
{
        p8 kept[HTTP_URL_MAX];
        positive length = string_length(location);
        string_address hash;

        if (length >= sizeof kept)
                return HTTP_BAD_URL;
        memory_copy(kept, location, length + 1);
        hash = string_first_of(kept, '#');
        if (hash)
                hash[0] = end;

        if (!string_compare_max(kept, (string_address) "https://", 8) ||
            !string_compare_max(kept, (string_address) "http://", 7))
        {
                if (string_length(kept) >= room)
                        return HTTP_BAD_URL;
                string_copy(into, kept);
                return HTTP_OK;
        }

        if (kept[0] == '/' && kept[1] == '/')
        {
                p8 address_to at = into;
                positive scheme_length = tls ? 6 : 5;
                positive rest = string_length(kept);

                if (scheme_length + rest + 1 > room)
                        return HTTP_BAD_URL;
                at = memory_copy_apart_end(at, tls ? "https:" : "http:",
                                           scheme_length);
                at = memory_copy_apart_end(at, kept, rest);
                at[0] = end;
                return HTTP_OK;
        }

        if (kept[0] == '/')
                return http_put_url(into, room, tls, host, port, kept);

        {
                p8 merged[HTTP_URL_MAX];
                string_address slash = string_last_of(path, '/');
                positive dir = slash ? (positive)(slash - path) + 1 : 1;
                positive used = 0;
                positive rest = string_length(kept);

                if (path[0] != '/')
                {
                        merged[0] = '/';
                        used = 1;
                }
                else
                {
                        if (dir >= sizeof merged)
                                return HTTP_BAD_URL;
                        memory_copy(merged, path, dir);
                        used = dir;
                }
                if (used + rest + 1 > sizeof merged)
                        return HTTP_BAD_URL;
                memory_copy(merged + used, kept, rest);
                used += rest;
                merged[used] = end;
                return http_put_url(into, room, tls, host, port, merged);
        }
}

static bipolar http_send_get(http_link address_to link, string_address host, p16 port,
                             string_address path, bool tls)
{
        p8 request[2048];
        p8 address_to into = request;
        positive path_length = string_length(path);
        positive host_length = string_length(host);
        bool named_port = (tls && port != HTTP_HTTPS_PORT) ||
                          (!tls && port != HTTP_PORT);
        p8 port_text[6];
        positive port_length = 0;
        positive used;

        if (named_port)
                port_length = http_digits(port_text, port);

        if (path_length + host_length + port_length + 96 >= sizeof request)
                return HTTP_BAD_URL;

        into = memory_copy_apart_end(into, "GET ", sizeof("GET ") - 1);
        into = memory_copy_apart_end(into, path, path_length);
        into = memory_copy_apart_end(into, " HTTP/1.1\r\nHost: ",
                                     sizeof(" HTTP/1.1\r\nHost: ") - 1);
        into = memory_copy_apart_end(into, host, host_length);
        if (named_port)
        {
                into = memory_copy_apart_end(into, ":", 1);
                into = memory_copy_apart_end(into, port_text, port_length);
        }
        into = memory_copy_apart_end(
            into,
            "\r\nUser-Agent: Wget\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            sizeof("\r\nUser-Agent: Wget\r\nAccept: */*\r\nConnection: close\r\n\r\n") -
                1);
        used = (positive)(into - request);
        return http_link_write(link, request, used);
}

static bipolar http_status_code(p8 address_to bytes, positive size, b32 address_to code)
{
        if (size < 13)
                return HTTP_NO_REPLY;
        if (string_compare_max(bytes, (string_address) "HTTP/1.", 7) ||
            bytes[7] < '0' || bytes[7] > '9' || bytes[8] != ' ' ||
            bytes[9] < '0' || bytes[9] > '9' || bytes[10] < '0' ||
            bytes[10] > '9' || bytes[11] < '0' || bytes[11] > '9' ||
            (bytes[12] != ' ' && bytes[12] != '\r' && bytes[12] != '\n'))
                return HTTP_MALFORMED;
        address_to code = (b32)((bytes[9] - '0') * 100 + (bytes[10] - '0') * 10 +
                                bytes[11] - '0');
        return HTTP_OK;
}

static p32 http_lookup(string_address host)
{
        bipolar server = string_to_host(host);
        p32 ip = 0;

        if (server >= 0)
                return (p32)server;
        if (dns_resolve_any((string_address) "/etc/resolv.conf", host, address_of ip,
                            3) != DNS_OK)
                return 0;
        return ip;
}

static fn http_url_leaf(string_address path, p8 address_to into, positive room)
{
        string_address query = string_first_of(path, '?');
        string_address slash;
        positive length;
        p8 kept[256];

        if (query)
        {
                length = (positive)(query - path);
                if (length >= sizeof kept)
                        length = sizeof kept - 1;
                memory_copy(kept, path, length);
                kept[length] = end;
                path = kept;
        }

        slash = string_last_of(path, '/');
        path = slash ? slash + 1 : path;
        if (!string_get(path))
                path = (string_address) "index.html";
        string_copy_max_end(into, path, room - 1);
}

static bipolar http_fetch_to(string_address start, bipolar dest, bool check_cert,
                             b32 address_to code)
{
        p8 url[HTTP_URL_MAX];
        positive hop;

        if (string_length(start) >= sizeof url)
                return HTTP_BAD_URL;
        string_copy(url, start);

        for (hop = 0; hop < HTTP_HOPS; hop++)
        {
                p8 host[256];
                p8 head[HTTP_HEAD_MAX];
                string_address path;
                p16 port = 80;
                bool tls = false;
                bool transfer_repeated = false;
                bool length_repeated = false;
                p32 ip;
                http_link link;
                http_body body;
                bipolar header;
                bipolar status;
                positive used = 0;
                positive value_length = 0;
                string_address value;
                string_address content_length;
                positive content_length_size = 0;
                b32 answer = 0;

                status = http_split_into(url, host, sizeof host, address_of port,
                                         address_of path, address_of tls);
                if (status)
                        return status;

                ip = http_lookup(host);
                if (!ip)
                        return HTTP_NO_HOST;

                status = http_link_open(address_of link, ip, port, host, tls,
                                        check_cert);
                if (status)
                        return status;

                status = http_send_get(address_of link, host, port, path, tls);
                if (status)
                {
                        http_link_close(address_of link);
                        return status;
                }

                while (http_header_end(head, used) < 0)
                {
                        positive got = 0;

                        if (used == sizeof head)
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        status = http_link_read(address_of link, head + used,
                                                sizeof head - used, address_of got);
                        if (status || !got)
                        {
                                http_link_close(address_of link);
                                return HTTP_NO_REPLY;
                        }
                        used += got;
                }

                header = http_header_end(head, used);
                if (http_status_code(head, (positive)header, address_of answer))
                {
                        http_link_close(address_of link);
                        return HTTP_MALFORMED;
                }
                if (code)
                        address_to code = answer;

                body.link = address_of link;
                body.stash = head + header;
                body.stash_used = used - (positive)header;

                if (answer >= 300 && answer < 400)
                {
                        p8 next[HTTP_URL_MAX];
                        string_address location;
                        positive location_length = 0;

                        location = http_header(head, (positive)header,
                                               (string_address) "location",
                                               address_of location_length, null);
                        if (!location || !location_length)
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        {
                                p8 placed[HTTP_URL_MAX];
                                if (location_length >= sizeof placed)
                                {
                                        http_link_close(address_of link);
                                        return HTTP_BAD_URL;
                                }
                                memory_copy(placed, location, location_length);
                                placed[location_length] = end;
                                status = http_absolutize(tls, host, port, path, placed,
                                                         next, sizeof next);
                        }
                        http_link_close(address_of link);
                        if (status)
                                return status;
                        if (string_length(next) >= sizeof url)
                                return HTTP_BAD_URL;
                        string_copy(url, next);
                        continue;
                }

                if (answer >= 400)
                {
                        http_link_close(address_of link);
                        return HTTP_OK;
                }

                value = http_header(head, (positive)header,
                                    (string_address) "transfer-encoding",
                                    address_of value_length,
                                    address_of transfer_repeated);
                content_length = http_header(head, (positive)header,
                                             (string_address) "content-length",
                                             address_of content_length_size,
                                             address_of length_repeated);
                if (transfer_repeated || length_repeated || (value && content_length))
                {
                        http_link_close(address_of link);
                        return HTTP_MALFORMED;
                }

                if (value)
                {
                        if (value_length < 7 ||
                            memory_compare_ascii_case(value, "chunked", 7) ||
                            string_span_max(value + 7, value_length - 7,
                                            string_set_blanks) != value_length - 7)
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        status = http_copy_chunked(address_of body, dest);
                }
                else if (content_length)
                {
                        string_address cursor = content_length;
                        positive said = 0;
                        positive at;

                        if (!string_digits_checked(address_of cursor, 10,
                                                   address_of said))
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        at = (positive)(cursor - content_length);
                        at += string_span_max(cursor, content_length_size - at,
                                              string_set_blanks);
                        if (at != content_length_size)
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        status = http_copy_n(address_of body, dest, said);
                }
                else
                        status = http_copy_all(address_of body, dest);

                http_link_close(address_of link);
                return status;
        }

        return HTTP_REDIRECTS;
}

#endif // STANDARD_MODERN_C_NET_HTTP
