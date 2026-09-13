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
#include "wait.c"

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
#define HTTP_FETCH_MAX (16 * 1024 * 1024)
#define HTTP_HOPS 10
#define HTTP_IDLE_SECONDS 30

#define HTTP_OK 0
#define HTTP_BAD_URL (-1)
#define HTTP_NO_HOST (-2)
#define HTTP_NO_ROUTE (-3)
#define HTTP_NO_REPLY (-4)
#define HTTP_MALFORMED (-5)
#define HTTP_NOT_PLAIN (-6)
#define HTTP_TLS (-7)
#define HTTP_REDIRECTS (-8)
#define HTTP_DOWNGRADE (-9)

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

        length = string_span_without_set(at, "/:?#");
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
                bound = string_span_without_set(at, "/?#");
                digits = at;

                if (!bound ||
                    !string_digits_checked(address_of digits, 10,
                                           address_of value) ||
                    (positive)(digits - at) != bound || value > 65535)
                        return HTTP_BAD_URL;

                address_to port = (p16)value;
                at = digits;
        }

        address_to path = string_get(at) && !string_is(at, '#')
                              ? at : (string_address) "/";

        return HTTP_OK;
}

/* Turn the path/query part of a URL into HTTP's origin form.  Fragments are
   local navigation state and must never cross the request boundary; a bare
   query still needs the root slash on the wire. */
static bipolar http_origin_form(string_address path, p8 address_to into,
                                positive room)
{
        string_address hash;
        positive length;
        bool root;

        if (!path)
                return HTTP_BAD_URL;

        hash = string_first_of(path, '#');
        length = hash ? (positive)(hash - path) : string_length(path);
        if (!length)
        {
                if (room < 2)
                        return HTTP_BAD_URL;
                into[0] = '/';
                into[1] = end;
                return HTTP_OK;
        }

        root = path[0] == '?';
        if (path[0] != '/' && !root)
                return HTTP_BAD_URL;
        if (length > room || root >= room - length)
                return HTTP_BAD_URL;

        if (root)
                *into++ = '/';
        memory_copy_end(into, path, length);
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

static bool http_token_byte(p8 byte)
{
        return (byte >= '0' && byte <= '9') ||
               (byte >= 'A' && byte <= 'Z') ||
               (byte >= 'a' && byte <= 'z') ||
               byte == '!' || byte == '#' || byte == '$' || byte == '%' ||
               byte == '&' || byte == '\'' || byte == '*' || byte == '+' ||
               byte == '-' || byte == '.' || byte == '^' || byte == '_' ||
               byte == '`' || byte == '|' || byte == '~';
}

static bool http_chunk_extensions_valid(string_address at,
                                        string_address stop)
{
        while (at < stop)
        {
                string_address name;

                if (*at++ != ';')
                        return false;
                name = at;
                while (at < stop && http_token_byte(*at))
                        at++;
                if (at == name)
                        return false;

                if (at < stop && *at == '=')
                {
                        at++;
                        if (at == stop)
                                return false;
                        if (*at == '"')
                        {
                                bool closed = false;

                                at++;
                                while (at < stop)
                                {
                                        p8 byte = *at++;

                                        if (byte == '"')
                                        {
                                                closed = true;
                                                break;
                                        }
                                        if (byte == '\\')
                                        {
                                                if (at == stop)
                                                        return false;
                                                byte = *at++;
                                        }
                                        if ((byte < 0x20 && byte != '\t') ||
                                            byte == 0x7f)
                                                return false;
                                }
                                if (!closed)
                                        return false;
                        }
                        else
                        {
                                string_address value = at;

                                while (at < stop && http_token_byte(*at))
                                        at++;
                                if (at == value)
                                        return false;
                        }
                }
        }

        return true;
}

/* Parse one complete chunk-size line for both buffered fetch and streaming
   wget.  Keeping the extension grammar here prevents the two paths from
   disagreeing about controls or ambiguous separators. */
static bipolar http_chunk_line(p8 address_to line, positive line_length,
                               positive address_to chunk_length)
{
        string_address number = (string_address)line;
        string_address stop = (string_address)(line + line_length);
        positive parsed;

        if (!line_length || stop[-1] != '\n')
                return HTTP_MALFORMED;
        stop--;
        if (stop > number && stop[-1] == '\r')
                stop--;

        if (!string_digits_checked(address_of number, 16, address_of parsed))
                return HTTP_MALFORMED;
        if (number < stop && byte_is_blank(number[0]))
                number += string_span_max(number, stop - number,
                                          string_set_blanks);
        if (number < stop)
        {
                if (!http_chunk_extensions_valid(number, stop))
                        return HTTP_MALFORMED;
        }

        address_to chunk_length = parsed;
        return HTTP_OK;
}

/* Return one for the terminating blank line, zero for a valid trailer field,
   and a negative status for malformed framing. */
static bipolar http_trailer_line(p8 address_to line, positive line_length)
{
        positive stop = line_length;
        positive colon = 0;

        if (!stop || line[stop - 1] != '\n')
                return HTTP_MALFORMED;
        stop--;
        if (stop && line[stop - 1] == '\r')
                stop--;
        if (!stop)
                return 1;

        while (colon < stop && line[colon] != ':')
        {
                if (!http_token_byte(line[colon]))
                        return HTTP_MALFORMED;
                colon++;
        }
        if (!colon || colon == stop)
                return HTTP_MALFORMED;

        for (positive at = colon + 1; at < stop; at++)
                if ((line[at] < 0x20 && line[at] != '\t') ||
                    line[at] == 0x7f)
                        return HTTP_MALFORMED;

        return 0;
}

static bipolar http_unchunk(p8 address_to bytes, positive size);

/*
        The whole exchange.

        The caller is handed the body and the status line's code. Redirects
        are reported rather than followed: a client that follows them needs a
        loop limit, a same-host rule and an opinion about relative locations,
        and none of that belongs in the first version.
*/
static bipolar http_status_code(p8 address_to bytes, positive size,
                                 b32 address_to code);

#define HTTP_BODY_CLOSE 0
#define HTTP_BODY_LENGTH 1
#define HTTP_BODY_CHUNKED 2

typedef struct
{
        b32 code;
        p8 body_kind;
        positive body_length;
        string_address location;
        positive location_length;
} http_response;

/* Status and body framing have one interpretation in both clients.  This
   rejects duplicate or conflicting declarations before either the buffered
   or streaming body path acts on them. */
static bipolar http_response_framing(p8 address_to bytes, positive size,
                                     positive address_to header_length,
                                     http_response address_to response)
{
        positive scan = size;
        positive at = 0;

        if (!size)
                return HTTP_NO_REPLY;
        if (scan > HTTP_HEAD_MAX)
                scan = HTTP_HEAD_MAX;

        for (;;)
        {
                bool transfer_repeated = false;
                bool length_repeated = false;
                positive value_length = 0;
                positive content_length_size = 0;
                string_address transfer;
                string_address content_length;
                bipolar header;

                memory_fill(response, 0, sizeof(*response));
                {
                        bipolar status = http_status_code(
                            bytes + at, scan - at,
                            address_of response->code);
                        if (status)
                                return status == HTTP_NO_REPLY &&
                                               scan == HTTP_HEAD_MAX
                                           ? HTTP_MALFORMED
                                           : status;
                }
                if (response->code < 100 || response->code > 599)
                        return HTTP_MALFORMED;

                header = http_header_end(bytes + at, scan - at);
                if (header < 0)
                        return scan == HTTP_HEAD_MAX ? HTTP_MALFORMED
                                                     : HTTP_NO_REPLY;

                transfer = http_header(
                    bytes + at, (positive)header,
                    (string_address)"transfer-encoding",
                    address_of value_length, address_of transfer_repeated);
                content_length = http_header(
                    bytes + at, (positive)header,
                    (string_address)"content-length",
                    address_of content_length_size, address_of length_repeated);
                if (transfer_repeated || length_repeated ||
                    (transfer && content_length))
                        return HTTP_MALFORMED;

                if (transfer)
                {
                        if (value_length < 7 ||
                            memory_compare_ascii_case(transfer, "chunked", 7) ||
                            string_span_max(transfer + 7, value_length - 7,
                                            string_set_blanks) !=
                                value_length - 7)
                                return HTTP_MALFORMED;
                        response->body_kind = HTTP_BODY_CHUNKED;
                }
                else if (content_length)
                {
                        string_address cursor = content_length;
                        positive digits;

                        if (!string_digits_checked(
                                address_of cursor, 10,
                                address_of response->body_length))
                                return HTTP_MALFORMED;
                        digits = (positive)(cursor - content_length);
                        digits += string_span_max(
                            cursor, content_length_size - digits,
                            string_set_blanks);
                        if (digits != content_length_size)
                                return HTTP_MALFORMED;
                        response->body_kind = HTTP_BODY_LENGTH;
                }

                /* Informational responses precede, rather than replace, the
                   final response.  They cannot carry message framing, and a
                   protocol switch is outside this connection-close client. */
                if (response->code < 200)
                {
                        if (response->code == 101 ||
                            response->body_kind != HTTP_BODY_CLOSE)
                                return HTTP_MALFORMED;
                        at += (positive)header;
                        continue;
                }

                if (response->code < 400 && response->code >= 300)
                {
                        bool repeated = false;

                        response->location = http_header(
                            bytes + at, (positive)header,
                            (string_address)"location",
                            address_of response->location_length,
                            address_of repeated);
                        if (repeated)
                                return HTTP_MALFORMED;
                }

                address_to header_length = at + (positive)header;
                return HTTP_OK;
        }
}

static bipolar http_get_request(p8 address_to request, positive room,
                                string_address host, p16 port,
                                string_address path, bool tls,
                                p8 version_minor, string_address agent,
                                positive address_to used);

static bipolar http_stream_open(p32 host, p16 port)
{
        socket_address_internet where = {
            .family = AF_INET, .port = network_order_16(port),
            .host = network_order_32(host)};
        bipolar handle = socket_new(AF_INET, SOCK_STREAM, 0);

        if (handle < 0)
                return HTTP_NO_ROUTE;
        if (!network_stream_timeout(handle, HTTP_IDLE_SECONDS, 0) ||
            socket_connect((b32)handle, address_of where, sizeof where) < 0)
        {
                socket_close((b32)handle);
                return HTTP_NO_ROUTE;
        }
        return handle;
}

static bipolar http_get(p32 host, p16 port, string_address name,
                        string_address path, http_buffer address_to body,
                        b32 address_to code)
{
        http_buffer whole = {0};
        bipolar handle;
        positive header = 0;
        bipolar status = HTTP_MALFORMED;
        positive length = 0;
        http_response response;

        handle = http_stream_open(host, port);
        if (handle < 0)
                return handle;

        {
                p8 request[2048];
                positive used = 0;

                status = http_get_request(
                    request, sizeof request, name, port, path, false, '0',
                    (string_address)"dawning", address_of used);
                if (status)
                {
                        goto done;
                }
                if (system_write_all((positive)handle, request, used) != used)
                {
                        status = HTTP_NO_REPLY;
                        goto done;
                }
        }

        {
                bipolar read = file_store_read_limit(
                    (positive)handle, address_of whole, HTTP_FETCH_MAX);

                status = read == -27 ? HTTP_MALFORMED
                                     : read < 0 ? HTTP_NO_REPLY : HTTP_OK;
        }
        if (status)
                goto done;
        status = HTTP_MALFORMED;

        socket_close((b32)handle);
        handle = -1;

        status = http_response_framing(whole.bytes, whole.used,
                                       address_of header,
                                       address_of response);
        /* The buffered reader has already reached EOF.  Once a status line
           exists, an incomplete header or an interim response without a final
           response is malformed rather than something more bytes can repair. */
        if (status == HTTP_NO_REPLY && whole.used >= 13)
                status = HTTP_MALFORMED;
        if (status)
                goto done;
        if (code)
                address_to code = response.code;
        status = HTTP_MALFORMED;
        length = whole.used - (positive)header;

        if (response.body_kind == HTTP_BODY_CHUNKED)
        {
                bipolar plain = http_unchunk(whole.bytes + header, length);
                if (plain < 0)
                        goto done;
                length = (positive)plain;
        }
        else if (response.body_kind == HTTP_BODY_LENGTH)
        {
                if (response.body_length > length)
                        goto done;
                length = response.body_length;
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
        // Streaming reuses the consumed header buffer for split lines and I/O.
        p8 address_to scratch;
        // A memory body compacts payload behind its read cursor.
        p8 address_to output;
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
        memory_fill(link, 0, sizeof(*link));
        link->handle = http_stream_open(ip, port);
        if (link->handle < 0)
                return link->handle;
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
                body->stash += take;
                body->stash_used -= take;
                address_to got = take;
                return HTTP_OK;
        }

        if (body->link)
                return http_link_read(body->link, into, room, got);
        *got = 0;
        return HTTP_OK;
}

/* One transfer loop for exact lengths, EOF bodies and in-place decoding.
   Stashed payload is already contiguous; only fresh socket data needs bounce. */
static bipolar http_copy(http_body address_to body, bipolar dest, positive want,
                          bool exact)
{
        if (!body->link && exact && want > body->stash_used)
                return HTTP_NO_REPLY;
        while (want)
        {
                p8 address_to data = body->scratch;
                positive take = min(want, (positive)8192);
                positive got = 0;

                if (body->stash_used)
                {
                        data = body->stash;
                        got = min(take, body->stash_used);
                        body->stash += got;
                        body->stash_used -= got;
                }
                else if (http_body_read(body, data, take, address_of got))
                        return HTTP_NO_REPLY;
                if (!got)
                        return exact ? HTTP_NO_REPLY : HTTP_OK;
                if (!body->link)
                {
                        memory_copy(body->output, data, got);
                        body->output += got;
                }
                else if (system_write_all((positive)dest, data, got) != got)
                        return HTTP_NO_REPLY;
                want -= got;
        }
        return HTTP_OK;
}

/* Borrow complete framing lines from the current span. Socket lines split
   across reads use the caller's scratch and retain the streaming line limit;
   complete memory responses have their existing whole-response bound. */
static bipolar http_line(http_body address_to body, positive limit,
                          p8 address_to address_to line,
                          positive address_to length)
{
        positive used = body->stash_used;
        positive span = memory_span_without_byte(body->stash, '\n', used);
        if (span < used && (!body->link || span < limit))
        {
                *line = body->stash;
                *length = span + 1;
                body->stash += span + 1;
                body->stash_used -= span + 1;
                return HTTP_OK;
        }
        if (!body->link || used >= limit)
                return HTTP_MALFORMED;

        p8 address_to scratch = body->scratch;
        memory_copy(scratch, body->stash, used);
        body->stash_used = 0;
        while (used < limit)
        {
                positive got = 0;
                if (http_link_read(body->link, scratch + used, limit - used,
                                    address_of got))
                        return HTTP_NO_REPLY;
                if (!got)
                        return HTTP_MALFORMED;
                span = memory_span_without_byte(scratch + used, '\n', got);
                if (span < got)
                {
                        *line = scratch;
                        *length = used + span + 1;
                        body->stash = scratch + *length;
                        body->stash_used = got - span - 1;
                        return HTTP_OK;
                }
                used += got;
        }
        return HTTP_MALFORMED;
}

static bipolar http_body_byte(http_body address_to body, p8 address_to byte)
{
        positive got = 0;
        return http_body_read(body, byte, 1, address_of got) || !got
                   ? HTTP_MALFORMED : HTTP_OK;
}

static bipolar http_copy_trailers(http_body address_to body)
{
        p8 address_to line;
        positive total = 0;

        while (total < HTTP_HEAD_MAX)
        {
                positive line_length = 0;
                bipolar parsed;

                if (http_line(body, HTTP_HEAD_MAX, address_of line,
                              address_of line_length))
                        return HTTP_MALFORMED;
                if (line_length > HTTP_HEAD_MAX - total)
                        return HTTP_MALFORMED;
                total += line_length;
                parsed = http_trailer_line(line, line_length);
                if (parsed < 0)
                        return parsed;
                if (parsed)
                        return HTTP_OK;
        }

        return HTTP_MALFORMED;
}

static bipolar http_copy_chunked(http_body address_to body, bipolar dest)
{
        p8 address_to line;

        for (;;)
        {
                positive line_length = 0;
                positive size = 0;
                p8 delimiter;

                if (http_line(body, 127, address_of line,
                              address_of line_length))
                        return HTTP_MALFORMED;
                if (http_chunk_line(line, line_length, address_of size))
                        return HTTP_MALFORMED;
                if (!size)
                        return http_copy_trailers(body);
                if (http_copy(body, dest, size, true))
                        return HTTP_NO_REPLY;
                if (http_body_byte(body, address_of delimiter))
                        return HTTP_MALFORMED;
                if (delimiter == '\n')
                        continue;
                if (delimiter != '\r' ||
                    http_body_byte(body, address_of delimiter) ||
                    delimiter != '\n')
                        return HTTP_MALFORMED;
        }
}


/* The memory frontend retains strict whole-body consumption and transfers no
   allocation: the same chunk decoder compacts within the response store. */
static bipolar http_unchunk(p8 address_to bytes, positive size)
{
        http_body body = {.stash = bytes, .stash_used = size, .output = bytes};
        if (http_copy_chunked(address_of body, -1) || body.stash_used)
                return HTTP_MALFORMED;
        return (bipolar)(body.output - bytes);
}


static bipolar http_get_request(p8 address_to request, positive room,
                                string_address host, p16 port,
                                string_address path, bool tls,
                                p8 version_minor, string_address agent,
                                positive address_to used)
{
        static const p8 version[] = " HTTP/1.";
        static const p8 host_label[] = "\r\nHost: ";
        static const p8 agent_label[] = "\r\nUser-Agent: ";
        static const p8 tail[] =
            "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        p8 target[HTTP_URL_MAX];
        positive host_length = string_length(host);
        bipolar normalized = http_origin_form(path, target, sizeof target);
        positive path_length = normalized ? 0 : string_length(target);
        positive agent_length = string_length(agent);
        bool named_port = (tls && port != HTTP_HTTPS_PORT) ||
                          (!tls && port != HTTP_PORT);
        p8 port_text[6];
        positive port_length = named_port ? positive_into(port_text, port) : 0;
        positive fixed = sizeof("GET ") - 1 + sizeof(version) - 1 + 1 +
                         sizeof(host_label) - 1 + sizeof(agent_label) - 1 +
                         sizeof(tail) - 1 + (named_port ? 1 : 0);
        p8 address_to into = request;

        if (normalized || version_minor < '0' || version_minor > '9' ||
            fixed > room ||
            path_length > room - fixed ||
            host_length > room - fixed - path_length ||
            agent_length > room - fixed - path_length - host_length ||
            port_length >
                room - fixed - path_length - host_length - agent_length)
                return HTTP_BAD_URL;

        into = memory_copy_apart_end(into, "GET ", sizeof("GET ") - 1);
        into = memory_copy_apart_end(into, target, path_length);
        into = memory_copy_apart_end(into, version, sizeof(version) - 1);
        *into++ = version_minor;
        into = memory_copy_apart_end(into, host_label,
                                     sizeof(host_label) - 1);
        into = memory_copy_apart_end(into, host, host_length);
        if (named_port)
        {
                *into++ = ':';
                into = memory_copy_apart_end(into, port_text, port_length);
        }
        into = memory_copy_apart_end(into, agent_label,
                                     sizeof(agent_label) - 1);
        into = memory_copy_apart_end(into, agent, agent_length);
        into = memory_copy_apart_end(into, tail, sizeof(tail) - 1);
        address_to used = (positive)(into - request);
        return HTTP_OK;
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
                port_length = 1 + positive_into(port_text, port);
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
        p8 base[HTTP_URL_MAX];
        positive length = string_length(location);
        string_address hash;

        if (length >= sizeof kept ||
            http_origin_form(path, base, sizeof base))
                return HTTP_BAD_URL;
        memory_copy(kept, location, length + 1);
        hash = string_first_of(kept, '#');
        if (hash)
                hash[0] = end;

        /* A fragment-only reference identifies the current resource.  The
           fragment itself was removed above; retain both path and query. */
        if (!kept[0])
                return http_put_url(into, room, tls, host, port, base);

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

        if (kept[0] == '?')
        {
                p8 merged[HTTP_URL_MAX];
                string_address query = string_first_of(base, '?');
                positive used = query ? (positive)(query - base)
                                      : string_length(base);
                positive rest = string_length(kept);

                if (used + rest + 1 > sizeof merged)
                        return HTTP_BAD_URL;
                memory_copy(merged, base, used);
                memory_copy_apart_end(merged + used, kept, rest);
                return http_put_url(into, room, tls, host, port, merged);
        }

        {
                p8 merged[HTTP_URL_MAX];
                string_address query = string_first_of(base, '?');
                string_address slash;
                positive dir;
                positive used = 0;
                positive rest = string_length(kept);

                if (query)
                        query[0] = end;
                slash = string_last_of(base, '/');
                dir = slash ? (positive)(slash - base) + 1 : 1;
                if (dir >= sizeof merged)
                        return HTTP_BAD_URL;
                memory_copy(merged, base, dir);
                used = dir;
                if (used + rest + 1 > sizeof merged)
                        return HTTP_BAD_URL;
                memory_copy(merged + used, kept, rest);
                used += rest;
                merged[used] = end;
                return http_put_url(into, room, tls, host, port, merged);
        }
}

/* Once a redirect chain has reached HTTPS, no later Location may discard
   transport authentication.  The caller applies this before name lookup or
   opening the next connection. */
static bool http_transport_allowed(bool address_to secure, bool tls)
{
        if (address_to secure && !tls)
                return false;
        address_to secure |= tls;
        return true;
}

static bipolar http_send_get(http_link address_to link, string_address host, p16 port,
                             string_address path, bool tls)
{
        p8 request[2048];
        positive used = 0;
        bipolar built = http_get_request(
            request, sizeof request, host, port, path, tls, '1',
            (string_address)"Wget", address_of used);

        if (built)
                return built;
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
        if (code)
                address_to code = (b32)((bytes[9] - '0') * 100 +
                                        (bytes[10] - '0') * 10 +
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
        p8 target[HTTP_URL_MAX];
        string_address query;
        string_address slash;

        if (!room)
                return;
        if (http_origin_form(path, target, sizeof target))
        {
                into[0] = end;
                return;
        }

        query = string_first_of(target, '?');
        if (query)
                query[0] = end;

        slash = string_last_of(target, '/');
        path = slash ? slash + 1 : target;
        if (!string_get(path))
                path = (string_address) "index.html";
        string_copy_max_end(into, path, room - 1);
}

static bipolar http_fetch_to(string_address start, bipolar dest, bool check_cert,
                             b32 address_to code)
{
        p8 url[HTTP_URL_MAX];
        positive hop;
        bool secure = false;

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
                p32 ip;
                http_link link;
                http_body body;
                http_response response;
                positive header = 0;
                bipolar status;
                positive used = 0;
                b32 answer = 0;

                status = http_split_into(url, host, sizeof host, address_of port,
                                         address_of path, address_of tls);
                if (status)
                        return status;
                if (!http_transport_allowed(address_of secure, tls))
                        return HTTP_DOWNGRADE;

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

                for (;;)
                {
                        positive got = 0;

                        status = http_response_framing(
                            head, used, address_of header,
                            address_of response);
                        if (status != HTTP_NO_REPLY)
                                break;
                        if (used == sizeof head)
                        {
                                status = HTTP_MALFORMED;
                                break;
                        }
                        status = http_link_read(address_of link, head + used,
                                                sizeof head - used, address_of got);
                        if (status || !got)
                        {
                                status = HTTP_NO_REPLY;
                                break;
                        }
                        used += got;
                }

                if (status)
                {
                        http_link_close(address_of link);
                        return status;
                }
                answer = response.code;
                if (code)
                        address_to code = answer;

                body.link = address_of link;
                body.stash = head + header;
                body.stash_used = used - (positive)header;
                body.scratch = head;

                if (answer >= 300 && answer < 400)
                {
                        p8 next[HTTP_URL_MAX];

                        if (!response.location || !response.location_length)
                        {
                                http_link_close(address_of link);
                                return HTTP_MALFORMED;
                        }
                        {
                                p8 placed[HTTP_URL_MAX];
                                if (response.location_length >= sizeof placed)
                                {
                                        http_link_close(address_of link);
                                        return HTTP_BAD_URL;
                                }
                                memory_copy(placed, response.location,
                                            response.location_length);
                                placed[response.location_length] = end;
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

                if (response.body_kind == HTTP_BODY_CHUNKED)
                        status = http_copy_chunked(address_of body, dest);
                else if (response.body_kind == HTTP_BODY_LENGTH)
                        status = http_copy(address_of body, dest,
                                           response.body_length, true);
                else
                        status = http_copy(address_of body, dest, positive_max, false);

                http_link_close(address_of link);
                return status;
        }

        return HTTP_REDIRECTS;
}

#endif // STANDARD_MODERN_C_NET_HTTP
