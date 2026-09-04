/*
        The utilities that are neither text nor files.

        hostid names the current machine, dd copies with a block size and a
        count, diff says what changed between two files, ps says what is
        running. They share nothing with each other beyond not belonging
        anywhere else.

        The reference is the tool on the machine, not the standard. dd's
        summary, diff's choice of which of two identical lines to call the
        changed one, and the widths ps pads to are all decisions nobody writes
        down twice the same way, so they are taken from the bytes the real
        tool emits and from the source that emits them.
*/

/* dns.c is included by net.c below this file.  hostid deliberately reuses
   that small resolver instead of bringing libc/NSS or a second DNS client
   into the shell. */
static bipolar dns_resolve_any(string_address path, string_address name,
                               p32 address_to found, positive seconds);

// hostid ----------------------------------------------------

/* Linux gethostid first accepts the native four-byte /etc/hostid.  Without
   one, glibc derives the value from the first IPv4 address of the kernel host
   name and swaps its two 16-bit halves.  Keeping that order matters: an
   administrator-supplied ID must not unexpectedly depend on DNS. */
static p32 tools_hostid_value()
{
        p32 identity;
        bipolar handle = system_open_at(AT_FDCWD, "/etc/hostid",
                                        FILE_READ | O_CLOEXEC);

        if (handle >= 0)
        {
                bipolar got = system_read_retry((positive)handle,
                                                address_of identity,
                                                sizeof(identity));
                system_close(handle);

                if (got == sizeof(identity))
                        return identity;
        }

        file_machine facts;
        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0 ||
            !facts.node[0])
                return 0;

        p32 host;

        if (dns_resolve_any((string_address) "/etc/resolv.conf", facts.node,
                            address_of host, 1))
                return 0;

        /* dns_resolve_any returns the numeric host; gethostid operates on
           the native in_addr word before rotating its two halves. */
        p32 wire = network_order_32(host);
        return (wire << 16) | (wire >> 16);
}

static b32 tools_hostid()
{
        file_taking taking = {
            .program = (string_address) "hostid",
            .allowed = (string_address) "",
            .valued = (string_address) "",
        };

        text_begin("hostid");

        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.first < (positive)program_argument_count())
                return text_refuse(program_argument((b32)taking.first),
                                   "extra operand", 1);

        p8 digits[8];
        positive length = positive_into_base(digits, tools_hostid_value(), 16,
                                             false);

        for (positive i = length; i < sizeof(digits); i++)
                text_put_character('0');

        text_put(digits, length);
        text_put_character('\n');
        return text_done(0);
}

// logger ----------------------------------------------------

/*
        logger is a formatter in front of the socket layer, not another
        network stack.  The connected Unix/IPv4 sockets below are the same
        socket_new/socket_connect/socket_send floor used by DNS and HTTP, and
        input is the text reader used by the line tools.  One arena slice is
        retained for the whole invocation so neither a message nor a refill
        allocates.

        Linux credentials are deliberately not forged for --id.  The value
        belongs in PROCID/the traditional tag and is emitted there, but the
        SCM_CREDENTIALS extension which can impersonate another extant task
        as root is a privilege operation rather than syslog framing.  The
        receiving kernel still supplies the logger process's real credentials.
*/

#define LOGGER_DEFAULT_SIZE 1024
#define LOGGER_HEADER_ROOM 4096
#define LOGGER_FRAME_ROOM 64
#define LOGGER_UNIX_PATH 108

enum
{
        LOGGER_PROTOCOL_LOCAL,
        LOGGER_PROTOCOL_3164,
        LOGGER_PROTOCOL_5424,
};

enum
{
        LOGGER_TRANSPORT_ANY,
        LOGGER_TRANSPORT_DGRAM,
        LOGGER_TRANSPORT_STREAM,
};

typedef struct
{
        bipolar handle;
        positive priority;
        positive process;
        positive maximum;
        p8 protocol;
        p8 transport;
        bool protocol_given;
        bool octet_count;
        bool standard_error;
        bool no_action;
        bool socket_errors;
        bool skip_empty;
        bool priority_prefix;
        bool rfc_time;
        bool rfc_quality;
        bool rfc_host;
        string_address tag;
        string_address server;
        string_address port;
        string_address socket_path;
        string_address message_id;
        p8 login[FILE_NAME_MAX];
        p8 hostname[65];
        p8 address_to workspace;
        p8 address_to body;
} logger_control;

typedef struct
{
        p8 address_to bytes;
        positive used;
        positive room;
        bool failed;
} logger_builder;

static fn logger_build_bytes(logger_builder address_to build,
                             address_any bytes, positive length)
{
        if (build->failed || length > build->room - min(build->used, build->room))
        {
                build->failed = true;
                return;
        }

        memory_copy(build->bytes + build->used, bytes, length);
        build->used += length;
}

static fn logger_build_string(logger_builder address_to build,
                              string_address text)
{
        logger_build_bytes(build, text, string_length(text));
}

static fn logger_build_character(logger_builder address_to build, p8 character)
{
        logger_build_bytes(build, address_of character, 1);
}

static fn logger_build_positive(logger_builder address_to build, positive value)
{
        p8 digits[24];
        positive length = positive_into(digits, value);
        logger_build_bytes(build, digits, length);
}

static fn logger_build_padded(logger_builder address_to build, positive value,
                              positive width)
{
        p8 digits[24];
        positive length = positive_into_padded(digits, value, width, '0');
        logger_build_bytes(build, digits, length);
}

static bool logger_named(string_address given, string_address expected)
{
        return string_compare_folded(given, expected) == 0;
}

typedef struct
{
        string_address name;
        p8 value;
} logger_name;

static const logger_name logger_facilities[] = {
    {(string_address)"kern", 0},       {(string_address)"user", 8},
    {(string_address)"mail", 16},      {(string_address)"daemon", 24},
    {(string_address)"auth", 32},      {(string_address)"security", 32},
    {(string_address)"syslog", 40},    {(string_address)"lpr", 48},
    {(string_address)"news", 56},      {(string_address)"uucp", 64},
    {(string_address)"cron", 72},      {(string_address)"authpriv", 80},
    {(string_address)"ftp", 88},       {(string_address)"local0", 128},
    {(string_address)"local1", 136},   {(string_address)"local2", 144},
    {(string_address)"local3", 152},   {(string_address)"local4", 160},
    {(string_address)"local5", 168},   {(string_address)"local6", 176},
    {(string_address)"local7", 184},   {null, 0},
};

static const logger_name logger_levels[] = {
    {(string_address)"emerg", 0},   {(string_address)"panic", 0},
    {(string_address)"alert", 1},   {(string_address)"crit", 2},
    {(string_address)"err", 3},     {(string_address)"error", 3},
    {(string_address)"warning", 4}, {(string_address)"warn", 4},
    {(string_address)"notice", 5},  {(string_address)"info", 6},
    {(string_address)"debug", 7},   {null, 0},
};

static bipolar logger_decode(string_address word,
                             const logger_name address_to names)
{
        positive number;

        if (string_digits_exact(word, address_of number))
        {
                for (positive at = 0; names[at].name; at++)
                        if (names[at].value == number)
                                return (bipolar)number;
                return -1;
        }

        for (positive at = 0; names[at].name; at++)
                if (logger_named(word, names[at].name))
                        return names[at].value;

        return -1;
}

static bool logger_priority(string_address word, positive address_to priority)
{
        p8 facility_text[32];
        string_address dot = string_first_of(word, '.');
        bipolar facility = 8;
        string_address level_text = word;

        if (dot)
        {
                positive length = (positive)(dot - word);
                if (!length || length >= sizeof(facility_text))
                        return false;
                memory_copy(facility_text, word, length);
                facility_text[length] = end;
                facility = logger_decode(facility_text, logger_facilities);
                level_text = dot + 1;
        }

        bipolar level = logger_decode(level_text, logger_levels);
        if (facility < 0 || level < 0)
                return false;

        /* LOG_KERN cannot be originated by a user process. */
        if (!facility)
                facility = 8;
        address_to priority = ((positive)facility & 0xf8) |
                              ((positive)level & 7);
        return true;
}

static bool logger_size(string_address text, positive address_to size)
{
        positive digits;
        p64 value = string_digits_max(text, 20, address_of digits);
        if (!digits || digits == 20)
                return false;
        text += digits;

        p8 power = file_size_power(string_get(text), true);
        if (power)
        {
                text++;
                p64 base = 1024;
                if (string_is(text, 'B'))
                {
                        base = 1000;
                        text++;
                }
                else if (string_is(text, 'i') && string_is(text + 1, 'B'))
                        text += 2;

                while (power--)
                {
                        if (value > (p64)positive_max / base)
                                return false;
                        value *= base;
                }
        }

        if (string_get(text) || !value || value > TEXT_ARENA_BYTES -
                                                 LOGGER_HEADER_ROOM - 2)
                return false;

        address_to size = (positive)value;
        return true;
}

static bool logger_rfc_flags(logger_control address_to control,
                             string_address flags)
{
        while (flags && string_get(flags))
        {
                string_address comma = string_first_of(flags, ',');
                positive length = comma ? (positive)(comma - flags)
                                        : string_length(flags);

                if (length == 6 && !string_compare_max(flags, "notime", 6))
                {
                        control->rfc_time = false;
                        control->rfc_quality = false;
                }
                else if (length == 4 && !string_compare_max(flags, "notq", 4))
                        control->rfc_quality = false;
                else if (length == 6 && !string_compare_max(flags, "nohost", 6))
                        control->rfc_host = false;
                else
                        return false;

                flags += length;
                if (comma)
                        flags++;
        }

        return true;
}

static positive logger_timestamp_3164(logger_builder address_to build,
                                      p64 seconds)
{
        time_t stamp = (time_t)seconds;
        tm broken;
        p8 time_text[32];

        if (!localtime_r(address_of stamp, address_of broken))
                return 0;

        positive length = strftime(time_text, sizeof(time_text),
                                   (string_address)"%b %e %H:%M:%S",
                                   address_of broken);
        if (!length)
                return 0;
        logger_build_bytes(build, time_text, length);
        return length;
}

static positive logger_timestamp_5424(logger_builder address_to build,
                                      p64 seconds, positive nanoseconds)
{
        time_t stamp = (time_t)seconds;
        tm broken;
        p8 time_text[32];

        if (!localtime_r(address_of stamp, address_of broken))
                return 0;

        positive length = strftime(time_text, sizeof(time_text),
                                   (string_address)"%Y-%m-%dT%H:%M:%S",
                                   address_of broken);
        if (!length)
                return 0;
        logger_build_bytes(build, time_text, length);
        logger_build_character(build, '.');
        logger_build_padded(build, nanoseconds / 1000, 6);
        logger_build_string(build, "+00:00");
        return length + 13;
}

static bool logger_header(logger_control address_to control,
                          p8 address_to into, positive room,
                          positive priority, positive address_to length)
{
        logger_builder build = {.bytes = into, .room = room};
        p64 now[2] = {0, 0};
        if (control->protocol != LOGGER_PROTOCOL_5424 || control->rfc_time)
                system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                              (positive)now);

        logger_build_character(address_of build, '<');
        logger_build_positive(address_of build, priority);
        logger_build_character(address_of build, '>');

        if (control->protocol == LOGGER_PROTOCOL_5424)
        {
                logger_build_string(address_of build, "1 ");
                if (control->rfc_time)
                {
                        if (!logger_timestamp_5424(address_of build, now[0],
                                                   (positive)now[1]))
                                return false;
                }
                else
                        logger_build_character(address_of build, '-');

                logger_build_character(address_of build, ' ');
                logger_build_string(address_of build,
                                    control->rfc_host ? control->hostname
                                                      : (string_address)"-");
                logger_build_character(address_of build, ' ');
                logger_build_string(address_of build, control->tag);
                logger_build_character(address_of build, ' ');
                if (control->process)
                        logger_build_positive(address_of build, control->process);
                else
                        logger_build_character(address_of build, '-');
                logger_build_character(address_of build, ' ');
                logger_build_string(address_of build,
                                    control->message_id ? control->message_id
                                                        : (string_address)"-");
                logger_build_character(address_of build, ' ');
                if (control->rfc_quality)
                        logger_build_string(address_of build,
                            "[timeQuality tzKnown=\"1\" isSynced=\"0\"]");
                else
                        logger_build_character(address_of build, '-');
                logger_build_character(address_of build, ' ');
        }
        else
        {
                if (!logger_timestamp_3164(address_of build, now[0]))
                        return false;
                logger_build_character(address_of build, ' ');

                if (control->protocol == LOGGER_PROTOCOL_3164)
                {
                        positive host_length = string_length(control->hostname);
                        string_address dot = string_first_of(control->hostname, '.');
                        if (dot)
                                host_length = (positive)(dot - control->hostname);
                        logger_build_bytes(address_of build, control->hostname,
                                           host_length);
                        logger_build_character(address_of build, ' ');
                }

                positive tag_length = string_length(control->tag);
                if (control->protocol == LOGGER_PROTOCOL_3164 && tag_length > 200)
                        tag_length = 200;
                logger_build_bytes(address_of build, control->tag, tag_length);
                if (control->process)
                {
                        logger_build_character(address_of build, '[');
                        logger_build_positive(address_of build, control->process);
                        logger_build_character(address_of build, ']');
                }
                logger_build_string(address_of build, ": ");
        }

        if (build.failed)
                return false;
        address_to length = build.used;
        return true;
}

#if defined(LINUX) && !defined(KERNEL_MODE)
typedef struct
{
        p16 family;
        p8 path[LOGGER_UNIX_PATH];
} logger_unix_address;

static bipolar logger_connect_kind(logger_control address_to control,
                                   p8 transport)
{
        b32 kind = transport == LOGGER_TRANSPORT_STREAM ? SOCK_STREAM
                                                        : SOCK_DGRAM;
        bipolar handle = socket_new(control->server ? AF_INET : AF_UNIX,
                                    kind | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return handle;

        bipolar connected;
        if (control->server)
        {
                bipolar host = string_to_host(control->server);
                if (host < 0)
                {
                        p32 found;
                        if (dns_resolve_any((string_address)"/etc/resolv.conf",
                                            control->server, address_of found,
                                            3))
                        {
                                socket_close((b32)handle);
                                return -ERROR_NO_ENTRY;
                        }
                        host = (bipolar)found;
                }

                positive port = transport == LOGGER_TRANSPORT_STREAM ? 601 : 514;
                if (control->port &&
                    (!string_digits_exact(control->port, address_of port) ||
                     !port || port > 65535))
                {
                        socket_close((b32)handle);
                        return -ERROR_INVALID;
                }

                socket_address_internet where = {
                    .family = AF_INET,
                    .port = network_order_16((p16)port),
                    .host = network_order_32((p32)host),
                };
                connected = socket_connect((b32)handle, address_of where,
                                           sizeof(where));
        }
        else
        {
                logger_unix_address where = {.family = AF_UNIX};
                string_address path = control->socket_path
                                          ? control->socket_path
                                          : (string_address)"/dev/log";
                positive length = string_length(path);
                if (!length || length >= sizeof(where.path))
                {
                        socket_close((b32)handle);
                        return -ERROR_INVALID;
                }
                memory_copy(where.path, path, length + 1);
                connected = socket_connect((b32)handle, address_of where,
                                           sizeof(where));
        }

        if (connected < 0)
        {
                socket_close((b32)handle);
                return connected;
        }

        control->transport = transport;
        return handle;
}

static bool logger_connect(logger_control address_to control)
{
        if (control->handle >= 0)
                socket_close((b32)control->handle);
        control->handle = -1;

        if (control->no_action)
                return true;

        if (control->transport != LOGGER_TRANSPORT_STREAM)
                control->handle = logger_connect_kind(control,
                                                     LOGGER_TRANSPORT_DGRAM);
        if (control->handle < 0 && control->transport != LOGGER_TRANSPORT_DGRAM)
                control->handle = logger_connect_kind(control,
                                                     LOGGER_TRANSPORT_STREAM);

        if (control->handle >= 0)
                return true;

        if (control->server || control->socket_errors)
        {
                text_error(control->server ? control->server
                                           : control->socket_path,
                           "cannot connect to logging socket");
                return false;
        }
        return true;
}

static bool logger_send_once(logger_control address_to control,
                             p8 address_to bytes, positive length)
{
        if (control->no_action || control->handle < 0)
                return true;

        if (control->transport == LOGGER_TRANSPORT_DGRAM)
                return socket_send((b32)control->handle, bytes, length,
                                   MSG_NOSIGNAL, null, 0) == (bipolar)length;

        positive sent = 0;
        while (sent < length)
        {
                bipolar wrote = socket_send((b32)control->handle, bytes + sent,
                                            length - sent, MSG_NOSIGNAL,
                                            null, 0);
                if (wrote <= 0)
                        return false;
                sent += (positive)wrote;
        }
        return true;
}
#endif

static bool logger_emit(logger_control address_to control,
                        positive length, positive priority)
{
#if defined(LINUX) && !defined(KERNEL_MODE)
        p8 address_to content = control->workspace + LOGGER_FRAME_ROOM;
        positive header_length;

        if (!logger_header(control, content, LOGGER_HEADER_ROOM -
                                             LOGGER_FRAME_ROOM,
                           priority, address_of header_length))
        {
                text_error(null, "cannot construct syslog header");
                return false;
        }

        memory_copy(content + header_length, control->body, length);
        positive content_length = header_length + length;
        p8 address_to wire = content;
        positive wire_length = content_length;

        if (control->octet_count)
        {
                p8 digits[24];
                positive digit_count = positive_into(digits, content_length);
                wire = control->workspace;
                memory_copy(wire, digits, digit_count);
                wire[digit_count] = ' ';
                memory_copy(wire + digit_count + 1, content, content_length);
                wire_length += digit_count + 1;
        }
        else if (control->transport == LOGGER_TRANSPORT_STREAM)
                wire[wire_length++] = '\n';

        bool sent = logger_send_once(control, wire, wire_length);
        if (!sent)
        {
                if (logger_connect(control))
                        sent = logger_send_once(control, wire, wire_length);
                if (!sent)
                        text_error(null, "send message failed");
        }

        if (control->standard_error)
        {
                text_out_to(2);
                text_put(wire, wire_length);
                if (!wire_length || wire[wire_length - 1] != '\n')
                        text_put_character('\n');
                text_flush();
                text_out_to(1);
        }

        return sent;
#else
        (void)control;
        (void)length;
        (void)priority;
        return false;
#endif
}

static bool logger_message_id_valid(string_address message_id)
{
        if (!message_id || !string_get(message_id))
                return false;
        for (string_address at = message_id; string_get(at); at++)
                if (byte_is_space(string_get(at)))
                        return false;
        return true;
}

static bool logger_stream(logger_control address_to control,
                          string_address path)
{
        if (!text_open(path))
                return false;

        bool answer = true;
        positive default_priority = control->priority;
        while (text_line_next())
        {
                positive from = 0;
                positive priority = default_priority;

                if (control->priority_prefix && text_line_length >= 3 &&
                    text_line[0] == '<')
                {
                        positive at = 1;
                        positive prefixed = 0;
                        while (at < text_line_length && at <= 3 &&
                               byte_is_digit(text_line[at]))
                        {
                                prefixed = prefixed * 10 + text_line[at] - '0';
                                at++;
                        }
                        if (at < text_line_length && text_line[at] == '>' &&
                            at > 1 && prefixed <= 191)
                        {
                                if (!(prefixed & 0xf8))
                                        prefixed |= default_priority & 0xf8;
                                priority = prefixed;
                                from = at + 1;
                        }
                }

                positive available = text_line_length - from;
                if (!available)
                {
                        if (!control->skip_empty)
                                answer &= logger_emit(control, 0, priority);
                        continue;
                }

                while (available)
                {
                        positive take = min(available, control->maximum);
                        memory_copy(control->body, text_line + from, take);
                        answer &= logger_emit(control, take, priority);
                        from += take;
                        available -= take;
                }
        }

        if (text_input.failed)
                answer = false;
        text_close();
        return answer;
}

static bool logger_operands(logger_control address_to control, positive first)
{
        positive count = (positive)program_argument_count();
        positive used = 0;
        bool answer = true;

        for (positive index = first; index < count; index++)
        {
                string_address word = program_argument((b32)index);
                positive length = string_length(word);

                if (length > control->maximum)
                {
                        if (used)
                        {
                                answer &= logger_emit(control, used,
                                                      control->priority);
                                used = 0;
                        }
                        memory_copy(control->body, word, control->maximum);
                        answer &= logger_emit(control, control->maximum,
                                              control->priority);
                        continue;
                }

                positive need = length + (used ? 1 : 0);
                if (need > control->maximum - min(used, control->maximum))
                {
                        answer &= logger_emit(control, used, control->priority);
                        used = 0;
                }
                if (used)
                        control->body[used++] = ' ';
                memory_copy(control->body + used, word, length);
                used += length;
        }

        if (used)
                answer &= logger_emit(control, used, control->priority);
        return answer;
}

static const file_long logger_longs[] = {
    {(string_address)"skip-empty", 'e'},
    {(string_address)"file", 'f'},
    {(string_address)"id", 'I'},
    {(string_address)"priority", 'p'},
    {(string_address)"stderr", 's'},
    {(string_address)"size", 'S'},
    {(string_address)"tag", 't'},
    {(string_address)"socket", 'u'},
    {(string_address)"udp", 'd'},
    {(string_address)"tcp", 'T'},
    {(string_address)"server", 'n'},
    {(string_address)"port", 'P'},
    {(string_address)"no-act", 'A'},
    {(string_address)"octet-count", 'O'},
    {(string_address)"prio-prefix", 'q'},
    {(string_address)"rfc3164", '3'},
    {(string_address)"rfc5424", '4'},
    {(string_address)"msgid", 'm'},
    {(string_address)"socket-errors", 'E'},
    {(string_address)"journald", 'J'},
    {(string_address)"sd-id", 'D'},
    {(string_address)"sd-param", 'X'},
    {null, 0},
};

static bool logger_journald(logger_control address_to control,
                            string_address path)
{
#if defined(LINUX) && !defined(KERNEL_MODE)
        text_arena_used = 0;
        if (!text_open(path))
                return false;

        positive length;
        bool failed;
        p8 address_to entry = text_arena_read_all(text_input.handle, 65536,
                                                   address_of length,
                                                   address_of failed);
        text_close();
        if (!entry || failed)
                return false;

        if (control->standard_error)
        {
                text_out_to(2);
                text_put(entry, length);
                if (!length || entry[length - 1] != '\n')
                        text_put_character('\n');
                text_flush();
                text_out_to(1);
        }

        if (control->no_action)
                return true;

        control->socket_path = (string_address)"/run/systemd/journal/socket";
        control->transport = LOGGER_TRANSPORT_DGRAM;
        control->socket_errors = true;
        if (!logger_connect(control))
                return false;
        return logger_send_once(control, entry, length);
#else
        (void)control;
        (void)path;
        return false;
#endif
}

static b32 tools_logger()
{
        p8 chosen_transport = LOGGER_TRANSPORT_ANY;
        p8 chosen_protocol = 0;
        const file_supersede supersedes[] = {
            {(string_address)"dT", address_of chosen_transport},
            {(string_address)"34", address_of chosen_protocol},
            {null, null},
        };
        file_taking taking = {
            .program = (string_address)"logger",
            .allowed = (string_address)"efipSstudTnP",
            .valued = (string_address)"fpStunPEmDX",
            .optional = (string_address)"I4J",
            .long_optional = (string_address)"I4J",
            .longs = logger_longs,
            .supersedes = supersedes,
        };

        text_begin("logger");
        text_delimiter = '\n';
        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.flags & (FILE_FLAG('D') | FILE_FLAG('X')))
                return text_refuse(null,
                    "structured-data and signature fields are not supported", 1);

        logger_control control = {
            .handle = -1,
            .priority = 13,
            .maximum = LOGGER_DEFAULT_SIZE,
            .transport = LOGGER_TRANSPORT_ANY,
            .protocol = LOGGER_PROTOCOL_LOCAL,
            .rfc_time = true,
            .rfc_quality = true,
            .rfc_host = true,
            .skip_empty = (taking.flags & FILE_FLAG('e')) != 0,
            .standard_error = (taking.flags & FILE_FLAG('s')) != 0,
            .no_action = (taking.flags & FILE_FLAG('A')) != 0,
            .octet_count = (taking.flags & FILE_FLAG('O')) != 0,
            .priority_prefix = (taking.flags & FILE_FLAG('q')) != 0,
            .tag = file_option_value(address_of taking, 't'),
            .server = file_option_value(address_of taking, 'n'),
            .port = file_option_value(address_of taking, 'P'),
            .socket_path = file_option_value(address_of taking, 'u'),
            .message_id = file_option_value(address_of taking, 'm'),
        };

        if (chosen_transport == 'd')
                control.transport = LOGGER_TRANSPORT_DGRAM;
        else if (chosen_transport == 'T')
                control.transport = LOGGER_TRANSPORT_STREAM;

        if (chosen_protocol == '3')
        {
                control.protocol = LOGGER_PROTOCOL_3164;
                control.protocol_given = true;
        }
        else if (chosen_protocol == '4')
        {
                control.protocol = LOGGER_PROTOCOL_5424;
                control.protocol_given = true;
        }

        string_address priority_text = file_option_value(address_of taking, 'p');
        if (priority_text && !logger_priority(priority_text, address_of control.priority))
                return text_refuse(priority_text, "unknown priority", 1);

        string_address size_text = file_option_value(address_of taking, 'S');
        if (size_text && !logger_size(size_text, address_of control.maximum))
                return text_refuse(size_text, "invalid message size", 1);

        if (control.message_id && !logger_message_id_valid(control.message_id))
                return text_refuse(control.message_id,
                                   "message id cannot contain whitespace", 1);

        string_address socket_errors = file_option_value(address_of taking, 'E');
        if (socket_errors)
        {
                if (!string_compare(socket_errors, "on"))
                        control.socket_errors = true;
                else if (!string_compare(socket_errors, "off"))
                        control.socket_errors = false;
                else if (string_compare(socket_errors, "auto"))
                        return text_refuse(socket_errors,
                                           "invalid socket error mode", 1);
        }
        else
                control.socket_errors = control.standard_error || control.no_action;

        if (taking.flags & FILE_FLAG('i'))
                control.process = (positive)system_call(syscall(getpid));
        if (taking.flags & FILE_FLAG('I'))
        {
                string_address identity = file_option_value(address_of taking, 'I');
                if (identity)
                {
                        if (!string_digits_exact(identity, address_of control.process) ||
                            !control.process || control.process > p32_max)
                                return text_refuse(identity, "invalid process id", 1);
                }
                else
                        control.process = (positive)system_call(syscall(getpid));
        }

        if (control.server && !control.protocol_given)
                control.protocol = LOGGER_PROTOCOL_5424;

        string_address rfc_flags = file_option_value(address_of taking, '4');
        if (rfc_flags && !logger_rfc_flags(address_of control, rfc_flags))
                return text_refuse(rfc_flags, "unsupported RFC 5424 qualifier", 1);

        if (!control.tag)
        {
                positive user = (positive)system_call(syscall(geteuid));
                if (!file_user_name(user, control.login, sizeof(control.login)))
                        string_copy_max_end(control.login, "<someone>",
                                            sizeof(control.login) - 1);
                control.tag = control.login;
        }

        file_machine facts;
        memory_fill(address_of facts, 0, sizeof(facts));
        if (system_call_1(syscall(uname), (positive)address_of facts) >= 0 &&
            facts.node[0])
                string_copy_max_end(control.hostname, facts.node,
                                    sizeof(control.hostname) - 1);
        else
                string_copy_max_end(control.hostname, "-",
                                    sizeof(control.hostname) - 1);

        if (control.protocol == LOGGER_PROTOCOL_5424 &&
            string_length(control.tag) > 48)
                return text_refuse(control.tag, "tag is too long for RFC 5424", 1);

        if (taking.flags & FILE_FLAG('J'))
        {
                bool answer = logger_journald(
                    address_of control, file_option_value(address_of taking, 'J'));
#if defined(LINUX) && !defined(KERNEL_MODE)
                if (control.handle >= 0)
                        socket_close((b32)control.handle);
#endif
                return text_done(answer ? 0 : 1);
        }

#if !defined(LINUX) || defined(KERNEL_MODE)
        return text_refuse(null, "logging sockets require Linux", 1);
#else
        text_arena_used = 0;
        control.workspace = (p8 address_to)text_arena_take(
            LOGGER_HEADER_ROOM + control.maximum + 2);
        if (!control.workspace)
                return text_done(1);
        control.body = control.workspace + LOGGER_HEADER_ROOM;

        if (!logger_connect(address_of control))
                return text_done(1);

        bool answer;
        positive count = (positive)program_argument_count();
        if (taking.first < count)
                answer = logger_operands(address_of control, taking.first);
        else
                answer = logger_stream(address_of control,
                                       file_option_value(address_of taking, 'f'));

        if (control.handle >= 0)
                socket_close((b32)control.handle);
        return text_done(answer ? 0 : 1);
#endif
}

// Login records: who, users and pinky -----------------------

/* The common fields have one Linux layout through offset 336. x86 keeps the
   historic 32-bit session/time ABI and a 384-byte record; ARM64 and RISC-V
   use native 64-bit fields and a 400-byte record. Decode those few offsets
   explicitly rather than pretending the compiler's struct utmp is portable. */
#if X64 || X86
#define LOGIN_UTMP_SIZE 384
#define LOGIN_UTMP_SECONDS 340
#define LOGIN_UTMP_TIME_32 1
#else
#define LOGIN_UTMP_SIZE 400
#define LOGIN_UTMP_SECONDS 344
#define LOGIN_UTMP_TIME_32 0
#endif

typedef struct
{
        p16 type;
        p32 process;
        p8 line[32];
        p8 identity[4];
        p8 user[32];
        p8 host[256];
        b16 termination;
        b16 exit;
        b64 seconds;
} login_record;

#define LOGIN_UTMP_PATH "/var/run/utmp"
#define LOGIN_RUN_LEVEL 1
#define LOGIN_BOOT_TIME 2
#define LOGIN_NEW_TIME 3
#define LOGIN_INIT_PROCESS 5
#define LOGIN_LOGIN_PROCESS 6
#define LOGIN_USER_PROCESS 7
#define LOGIN_DEAD_PROCESS 8
#define LOGIN_ERROR_INTERRUPTED (-4)

typedef bool(address_to login_record_visit)(login_record address_to record);

/* One reader for all three applets.  It uses the already resident transfer
   block, preserves a record split across reads and copies into an aligned
   object before interpreting it.  A truncated last record is ignored, as the
   system readers do while a writer is extending utmp. */
static bool login_records(string_address path, bool check_processes,
                          login_record_visit visit)
{
        bipolar input;

        do
                input = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        while (input == LOGIN_ERROR_INTERRUPTED);

        if (input < 0)
        {
                if (input == -ERROR_NO_ENTRY)
                        return true;

                text_error(path, file_reason(input));
                return false;
        }

        positive held = 0;
        bool answer = true;

        for (;;)
        {
                bipolar got = system_read_retry((positive)input,
                                                file_transfer + held,
                                                sizeof(file_transfer) - held);

                if (got < 0)
                {
                        text_error(path, file_reason(got));
                        answer = false;
                        break;
                }

                if (!got)
                        break;

                positive have = held + (positive)got;
                positive at = 0;

                while (have - at >= LOGIN_UTMP_SIZE)
                {
                        p8 address_to bytes = file_transfer + at;
                        login_record record = {
                            .type = memory_load_unaligned(p16, bytes),
                            .process = memory_load_unaligned(p32, bytes + 4),
                            .termination = memory_load_unaligned(b16, bytes + 332),
                            .exit = memory_load_unaligned(b16, bytes + 334),
                            .seconds = LOGIN_UTMP_TIME_32
                                           ? (b64)memory_load_unaligned(b32,
                                                                        bytes + LOGIN_UTMP_SECONDS)
                                           : memory_load_unaligned(b64,
                                                                   bytes + LOGIN_UTMP_SECONDS),
                        };
                        memory_copy_apart(record.line, bytes + 8,
                                          sizeof(record.line));
                        memory_copy_apart(record.identity, bytes + 40,
                                          sizeof(record.identity));
                        memory_copy_apart(record.user, bytes + 44,
                                          sizeof(record.user));
                        memory_copy_apart(record.host, bytes + 76,
                                          sizeof(record.host));
                        at += LOGIN_UTMP_SIZE;

                        if (check_processes && record.type == LOGIN_USER_PROCESS &&
                            record.process)
                        {
                                bipolar alive = system_call_2(
                                    syscall(kill), (positive)record.process, 0);

                                if (alive == -ERROR_NO_PROCESS)
                                        continue;
                        }

                        if (!visit(address_of record))
                        {
                                answer = false;
                                goto done;
                        }
                }

                held = have - at;
                for (positive i = 0; i < held; i++)
                        file_transfer[i] = file_transfer[at + i];
        }

done:
        system_close((positive)input);
        return answer;
}

/* utmp strings need not contain a terminator.  Every consumer receives one,
   and users/who -q can additionally discard the historical space padding. */
static positive login_field(p8 address_to into, positive room,
                            p8 address_to source, positive width, bool trim)
{
        positive length = string_length_max(source, width);

        if (trim)
                while (length && (source[length - 1] == ' ' ||
                                  source[length - 1] == '\t'))
                        length--;

        if (length >= room)
                length = room - 1;

        memory_copy_apart(into, source, length);
        into[length] = end;
        return length;
}

static fn login_put_width(string_address value, positive length,
                          positive width, bool right)
{
        if (right && length < width)
                for (positive i = length; i < width; i++)
                        text_put_character(' ');

        text_put((p8 address_to)value, length);

        if (!right && length < width)
                for (positive i = length; i < width; i++)
                        text_put_character(' ');
}

static positive login_time(p8 address_to into, b32 seconds)
{
        b64 year;
        positive month, day, hour, minute, second;
        file_split_moment((b64)seconds, address_of year, address_of month,
                          address_of day, address_of hour, address_of minute,
                          address_of second);
        string_address named = file_month_names[month - 1];
        positive made = 0;

        into[made++] = byte_to_upper(named[0]);
        into[made++] = named[1];
        into[made++] = named[2];
        into[made++] = ' ';
        made += positive_into_padded(into + made, day, 2, ' ');
        into[made++] = ' ';
        made += positive_into_padded(into + made, hour, 2, '0');
        into[made++] = ':';
        made += positive_into_padded(into + made, minute, 2, '0');
        into[made] = end;
        return made;
}

typedef struct
{
        bool known;
        bool writable;
        b64 accessed;
} login_terminal;

static login_terminal login_terminal_facts(string_address line)
{
        login_terminal answer = {0};
        p8 path[FILE_PATH_MAX];
        string_address device = string_first_of(line, ' ');

        device = device ? device + 1 : line;
        if (!*device)
                return answer;

        if (string_is(device, '/'))
                string_copy_max_end(path, device, sizeof(path) - 1);
        else
        {
                memory_copy_apart(path, "/dev/", 5);
                string_copy_max_end(path + 5, device, sizeof(path) - 6);
        }

        file_facts facts;
        if (file_look_at(path, address_of facts))
        {
                answer.known = true;
                answer.writable = (facts.mode & 0020) != 0;
                answer.accessed = facts.accessed.seconds;
        }

        return answer;
}

static positive login_idle_who(p8 address_to into, login_terminal terminal,
                                b64 boot)
{
        if (!terminal.known)
                return memory_copy_apart_end(into, "  ?", 3) - into;

        b64 now = file_now();
        b64 idle = now - terminal.accessed;

        if (boot < terminal.accessed && terminal.accessed <= now &&
            idle < 86400)
        {
                if (idle < 60)
                        return memory_copy_apart_end(into, "  .  ", 5) - into;

                positive made = positive_into_padded(
                    into, (positive)idle / 3600, 2, '0');
                into[made++] = ':';
                made += positive_into_padded(
                    into + made, ((positive)idle / 60) % 60, 2, '0');
                into[made] = end;
                return made;
        }

        return memory_copy_apart_end(into, " old ", 5) - into;
}

static positive login_idle_pinky(p8 address_to into, login_terminal terminal)
{
        if (!terminal.known)
                return memory_copy_apart_end(into, "?????", 5) - into;

        b64 idle = file_now() - terminal.accessed;

        if (idle < 60)
                return memory_copy_apart_end(into, "     ", 5) - into;

        if (idle < 86400)
        {
                positive made = positive_into_padded(
                    into, (positive)idle / 3600, 2, '0');
                into[made++] = ':';
                made += positive_into_padded(
                    into + made, ((positive)idle / 60) % 60, 2, '0');
                into[made] = end;
                return made;
        }

        positive made = positive_into(into, (positive)(idle / 86400));
        into[made++] = 'd';
        into[made] = end;
        return made;
}

static positive login_signed(p8 address_to into, bipolar value)
{
        positive made = 0;
        positive magnitude;

        if (value < 0)
        {
                into[made++] = '-';
                magnitude = (positive)(-(value + 1)) + 1;
        }
        else
                magnitude = (positive)value;

        made += positive_into(into + made, magnitude);
        into[made] = end;
        return made;
}

static bool login_ends_with(string_address text, string_address suffix)
{
        positive length = string_length(text);
        positive tail = string_length(suffix);

        return tail <= length && !memory_compare(text + length - tail,
                                                  suffix, tail);
}

/* who ------------------------------------------------------ */

typedef struct
{
        bool users;
        bool boot;
        bool dead;
        bool login;
        bool init;
        bool runlevel;
        bool clock;
        bool heading;
        bool my_line;
        bool count;
        bool short_output;
        bool mesg;
        bool idle;
        bool exit;
        string_address tty;
        positive count_users;
        bool count_first;
        b64 boottime;
} login_who_options;

static login_who_options login_who;

static fn login_who_line(string_address user, p8 state, string_address line,
                         string_address time, string_address idle,
                         string_address pid, string_address comment,
                         string_address exit_text)
{
        positive user_length = string_length(user);
        positive line_length = string_length(line);
        positive time_length = string_length(time);
        positive idle_length = string_length(idle);
        positive pid_length = string_length(pid);
        positive comment_length = string_length(comment);
        positive exit_length = string_length(exit_text);

        login_put_width(user, user_length, 8, false);
        if (login_who.mesg)
        {
                text_put_character(' ');
                text_put_character(state);
        }
        text_put_character(' ');
        login_put_width(line, line_length, 12, false);
        text_put_character(' ');
        login_put_width(time, time_length, 12, false);

        bool later = idle_length || pid_length || comment_length || exit_length;
        if (login_who.idle && !login_who.short_output && later)
        {
                text_put_character(' ');
                login_put_width(idle, idle_length, 6, false);
        }
        if (!login_who.short_output &&
            (pid_length || comment_length || exit_length))
        {
                text_put_character(' ');
                login_put_width(pid, pid_length, 10, true);
        }
        if (comment_length || exit_length)
        {
                text_put_character(' ');
                login_put_width(comment, comment_length,
                                exit_length ? 8 : comment_length, false);
        }
        if (exit_length)
        {
                text_put_character(' ');
                text_put_string(exit_text);
        }
        text_put_character('\n');
}

static fn login_who_heading()
{
        login_who_line("NAME", ' ', "LINE", "TIME",
                       login_who.idle && !login_who.short_output ? "IDLE" : "",
                       !login_who.short_output ? "PID" : "", "COMMENT",
                       login_who.exit ? "EXIT" : "");
}

static bool login_who_visit(login_record address_to record)
{
        p8 user[33], line[33], host[257], time[24];
        login_field(user, sizeof(user), record->user, sizeof(record->user),
                    login_who.count);
        login_field(line, sizeof(line), record->line, sizeof(record->line),
                    false);
        login_field(host, sizeof(host), record->host, sizeof(record->host),
                    false);

        if (record->type == LOGIN_BOOT_TIME)
                login_who.boottime = (b64)record->seconds;

        if (login_who.my_line &&
            (!login_who.tty || !login_ends_with(line, login_who.tty)))
                return true;

        if (login_who.count)
        {
                if (record->type == LOGIN_USER_PROCESS)
                {
                        if (!login_who.count_first)
                                text_put_character(' ');
                        text_put_string(user);
                        login_who.count_first = false;
                        login_who.count_users++;
                }
                return true;
        }

        login_time(time, record->seconds);

        if (record->type == LOGIN_USER_PROCESS && login_who.users)
        {
                login_terminal terminal = login_terminal_facts(line);
                p8 idle[32] = {0};
                p8 pid[24] = {0};
                p8 comment[260] = {0};

                if (login_who.idle && !login_who.short_output)
                        login_idle_who(idle, terminal, login_who.boottime);
                if (!login_who.short_output)
                        positive_into_string(pid, record->process);
                if (host[0])
                {
                        positive length = string_length(host);
                        comment[0] = '(';
                        memory_copy_apart(comment + 1, host, length);
                        comment[length + 1] = ')';
                        comment[length + 2] = end;
                }

                login_who_line(user,
                               terminal.known
                                   ? terminal.writable ? '+' : '-'
                                   : '?',
                               line, time, idle, pid, comment, "");
                return true;
        }

        if (record->type == LOGIN_BOOT_TIME && login_who.boot)
                login_who_line("", ' ', "system boot", time, "", "", "", "");
        else if (record->type == LOGIN_NEW_TIME && login_who.clock)
                login_who_line("", ' ', "clock change", time, "", "", "", "");
        else if (record->type == LOGIN_RUN_LEVEL && login_who.runlevel)
        {
                p8 level[16] = "run-level ";
                p8 comment[8] = "last=";
                p8 current = (p8)record->process;
                p8 previous = (p8)(record->process >> 8);
                level[10] = current;
                level[11] = end;

                if (byte_is_printable(previous))
                {
                        comment[5] = previous == 'N' ? 'S' : previous;
                        comment[6] = end;
                }
                else
                        comment[0] = end;

                login_who_line("", ' ', level, time, "", "", comment, "");
        }
        else if ((record->type == LOGIN_LOGIN_PROCESS && login_who.login) ||
                 (record->type == LOGIN_INIT_PROCESS && login_who.init) ||
                 (record->type == LOGIN_DEAD_PROCESS && login_who.dead))
        {
                p8 pid[24], comment[16] = "id=";
                p8 exit_text[64] = {0};
                positive_into_string(pid, record->process);
                positive id_length = string_length_max(record->identity,
                                                        sizeof(record->identity));
                memory_copy_apart(comment + 3, record->identity, id_length);
                comment[3 + id_length] = end;

                if (record->type == LOGIN_DEAD_PROCESS)
                {
                        positive made = memory_copy_apart_end(
                                            exit_text, "term=", 5) - exit_text;
                        made += login_signed(exit_text + made,
                                             (b16)record->termination);
                        exit_text[made++] = ' ';
                        made += memory_copy_apart_end(exit_text + made,
                                                     "exit=", 5) -
                                (exit_text + made);
                        made += login_signed(exit_text + made,
                                             (b16)record->exit);
                        exit_text[made] = end;
                }

                login_who_line(record->type == LOGIN_LOGIN_PROCESS
                                   ? "LOGIN"
                                   : "",
                               ' ', line, time, "", pid, comment, exit_text);
        }

        return true;
}

static const file_long login_who_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "boot", 'b'},
    {(string_address) "count", 'q'},
    {(string_address) "dead", 'd'},
    {(string_address) "heading", 'H'},
    {(string_address) "login", 'l'},
    {(string_address) "message", 'T'},
    {(string_address) "mesg", 'T'},
    {(string_address) "process", 'p'},
    {(string_address) "runlevel", 'r'},
    {(string_address) "short", 's'},
    {(string_address) "time", 't'},
    {(string_address) "users", 'u'},
    {(string_address) "writable", 'T'},
    {null, 0},
};

static b32 tools_who()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "who",
            .allowed = (string_address) "abdlmpqrstuwHT",
            .valued = (string_address) "",
            .longs = login_who_longs,
            .operand = file_operand,
        };

        text_begin("who");
        memory_fill(address_of login_who, 0, sizeof(login_who));

        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);

        if (file_operand_count > 2)
                return text_refuse(file_operand_at(2), "extra operand", 1);

        positive flags = taking.flags;
        bool all = (flags & FILE_FLAG('a')) != 0;
        bool assumptions = !(flags & (FILE_FLAG('a') | FILE_FLAG('b') |
                                      FILE_FLAG('d') | FILE_FLAG('l') |
                                      FILE_FLAG('p') | FILE_FLAG('r') |
                                      FILE_FLAG('t') | FILE_FLAG('u')));

        login_who.users = assumptions || all || (flags & FILE_FLAG('u'));
        login_who.boot = all || (flags & FILE_FLAG('b'));
        login_who.dead = all || (flags & FILE_FLAG('d'));
        login_who.login = all || (flags & FILE_FLAG('l'));
        login_who.init = all || (flags & FILE_FLAG('p'));
        login_who.runlevel = all || (flags & FILE_FLAG('r'));
        login_who.clock = all || (flags & FILE_FLAG('t'));
        login_who.heading = (flags & FILE_FLAG('H')) != 0;
        login_who.my_line = (flags & FILE_FLAG('m')) != 0 ||
                            file_operand_count == 2;
        login_who.count = (flags & FILE_FLAG('q')) != 0;
        login_who.count_first = true;
        login_who.short_output = assumptions || (flags & FILE_FLAG('s'));
        login_who.mesg = all || (flags & (FILE_FLAG('T') | FILE_FLAG('w')));
        login_who.idle = all ||
                         (flags & (FILE_FLAG('d') | FILE_FLAG('l') |
                                   FILE_FLAG('r') | FILE_FLAG('u')));
        login_who.exit = all || (flags & FILE_FLAG('d'));

        if (login_who.exit)
                login_who.short_output = false;

        p8 tty[FILE_PATH_MAX];
        if (login_who.my_line && file_input_terminal_name(tty, sizeof(tty)) >= 0)
                login_who.tty = !string_compare_max(tty, "/dev/", 5)
                                    ? tty + 5
                                    : tty;

        if (login_who.heading && !login_who.count)
                login_who_heading();

        string_address path = file_operand_count == 1
                                  ? file_operand_at(0)
                                  : (string_address)LOGIN_UTMP_PATH;
        bool default_path = file_operand_count != 1;

        if (!login_records(path, default_path, login_who_visit))
                return text_done(1);

        if (login_who.count)
        {
                p8 count[24];
                positive length = positive_into(count, login_who.count_users);
                text_put_character('\n');
                text_put_string("# users=");
                text_put(count, length);
                text_put_character('\n');
        }

        return text_done(0);
}

/* users ---------------------------------------------------- */

typedef struct login_name login_name;
struct login_name
{
        login_name address_to next;
        p8 text[33];
};

static login_name address_to login_users_head;
static positive login_users_count;

static bipolar login_name_compare(string_address left, string_address right)
{
        while (*left && *left == *right)
        {
                left++;
                right++;
        }
        return (bipolar)*left - (bipolar)*right;
}

static bool login_users_visit(login_record address_to record)
{
        if (record->type != LOGIN_USER_PROCESS)
                return true;

        login_name address_to node =
            (login_name address_to)text_arena_take(sizeof(login_name));
        if (!node)
                return false;

        login_field(node->text, sizeof(node->text), record->user,
                    sizeof(record->user), true);
        login_name address_to address_to at = address_of login_users_head;

        while (*at && login_name_compare((*at)->text, node->text) <= 0)
                at = address_of (*at)->next;

        node->next = *at;
        *at = node;
        login_users_count++;
        return true;
}

static b32 tools_users()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "users",
            .allowed = (string_address) "",
            .valued = (string_address) "",
            .operand = file_operand,
        };

        text_begin("users");
        text_arena_used = 0;
        login_users_head = null;
        login_users_count = 0;

        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);
        if (file_operand_count > 1)
                return text_refuse(file_operand_at(1), "extra operand", 1);

        string_address path = file_operand_count
                                  ? file_operand_at(0)
                                  : (string_address)LOGIN_UTMP_PATH;
        bool default_path = !file_operand_count;

        if (!login_records(path, default_path, login_users_visit))
                return text_done(1);

        for (login_name address_to node = login_users_head; node;
             node = node->next)
        {
                text_put_string(node->text);
                text_put_character(node->next ? ' ' : '\n');
        }

        return text_done(0);
}

/* pinky ---------------------------------------------------- */

typedef struct
{
        bool short_output;
        bool heading;
        bool fullname;
        bool where;
        bool idle;
} login_pinky_options;

static login_pinky_options login_pinky;

static bool login_pinky_seen(p8 letter, string_address value)
{
        (void)value;
        if (letter == 's')
                login_pinky.short_output = true;
        else if (letter == 'l')
                login_pinky.short_output = false;
        return true;
}

static bool login_fullname(string_address name, p8 address_to into,
                           positive room)
{
        p8 address_to accounts = file_account_text(FILE_ACCOUNT_USER);
        positive at = 0;
        positive wanted = string_length(name);
        file_account_record record;

        while (file_account_next(accounts, address_of at, 4, address_of record))
        {
                if (record.name_length != wanted ||
                    memory_compare(record.name, name, wanted) ||
                    !record.has_value)
                        continue;

                positive made = 0;
                for (positive i = 0;
                     i < record.value_length && record.value[i] != ','; i++)
                {
                        if (record.value[i] != '&')
                        {
                                if (made + 1 < room)
                                        into[made++] = record.value[i];
                                continue;
                        }

                        for (positive j = 0; j < wanted && made + 1 < room; j++)
                                into[made++] = j ? name[j]
                                                   : byte_to_upper(name[j]);
                }

                into[made] = end;
                return true;
        }

        return false;
}

static fn login_pinky_heading()
{
        login_put_width("Login", 5, 8, false);
        if (login_pinky.fullname)
        {
                text_put_character(' ');
                login_put_width("Name", 4, 19, false);
        }
        text_put_character(' ');
        login_put_width(" TTY", 4, 9, false);
        if (login_pinky.idle)
        {
                text_put_character(' ');
                login_put_width("Idle", 4, 6, false);
        }
        text_put_character(' ');
        login_put_width("When", 4, 12, false);
        if (login_pinky.where)
        {
                text_put_character(' ');
                text_put_string("Where");
        }
        text_put_character('\n');
}

static bool login_pinky_visit(login_record address_to record)
{
        if (record->type != LOGIN_USER_PROCESS)
                return true;

        p8 user[33], line[33], host[257], time[24], idle[32];
        positive user_length = login_field(
            user, sizeof(user), record->user, sizeof(record->user), false);

        if (file_operand_count)
        {
                bool wanted = false;
                for (positive i = 0; i < file_operand_count; i++)
                        if (string_equals(user, file_operand_at(i)))
                        {
                                wanted = true;
                                break;
                        }
                if (!wanted)
                        return true;
        }

        positive line_length = login_field(
            line, sizeof(line), record->line, sizeof(record->line), false);
        positive host_length = login_field(
            host, sizeof(host), record->host, sizeof(record->host), false);
        positive time_length = login_time(time, record->seconds);
        login_terminal terminal = login_terminal_facts(line);

        login_put_width(user, user_length, user_length < 8 ? 8 : user_length,
                        false);

        if (login_pinky.fullname)
        {
                p8 fullname[256];
                bool known = login_fullname(user, fullname, sizeof(fullname));
                positive length = known ? string_length(fullname) : 3;
                if (length > 19)
                        length = 19;
                text_put_character(' ');
                login_put_width(known ? fullname : (string_address)"???",
                                length, 19, !known);
        }

        text_put_character(' ');
        text_put_character(terminal.known
                               ? terminal.writable ? ' ' : '*'
                               : '?');
        login_put_width(line, line_length, line_length < 8 ? 8 : line_length,
                        false);

        if (login_pinky.idle)
        {
                positive idle_length = login_idle_pinky(idle, terminal);
                text_put_character(' ');
                login_put_width(idle, idle_length, 6, false);
        }

        text_put_character(' ');
        text_put(time, time_length);

        if (login_pinky.where && host_length)
        {
                text_put_character(' ');
                text_put(host, host_length);
        }

        text_put_character('\n');
        return true;
}

static b32 tools_pinky()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "pinky",
            .allowed = (string_address) "sfwiqbhlp",
            .valued = (string_address) "",
            .operand = file_operand,
            .seen = login_pinky_seen,
        };

        text_begin("pinky");
        login_pinky = (login_pinky_options){
            .short_output = true,
            .heading = true,
            .fullname = true,
            .where = true,
            .idle = true,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);

        if (!login_pinky.short_output)
                return text_refuse(null, "long format is not supported", 1);

        positive flags = taking.flags;
        login_pinky.heading = !(flags & FILE_FLAG('f'));
        login_pinky.fullname =
            !(flags & (FILE_FLAG('w') | FILE_FLAG('i') | FILE_FLAG('q')));
        login_pinky.where = !(flags & (FILE_FLAG('i') | FILE_FLAG('q')));
        login_pinky.idle = !(flags & FILE_FLAG('q'));

        if (login_pinky.heading)
                login_pinky_heading();

        if (!login_records((string_address)LOGIN_UTMP_PATH, false,
                           login_pinky_visit))
                return text_done(1);

        return text_done(0);
}

// tsort -----------------------------------------------------

/* A node is named by a token kept in the input buffer.  Edges and all graph
   links are indexes, so a large relation set stays dense and carries no
   allocator metadata or pointer chasing beyond the successor walk itself. */
typedef struct
{
        string_address name;
        positive hash;
        positive predecessors;
        b32 top;
        b32 queue;
        bool printed;
} tsort_node;

typedef struct
{
        b32 successor;
        b32 next;
} tsort_edge;

static tsort_node address_to tsort_nodes;
static tsort_edge address_to tsort_edges;
static b32 address_to tsort_buckets;
static positive tsort_bucket_count;
static positive tsort_node_count;

#define TSORT_ERROR_INTERRUPTED (-4)

static PURE bipolar tsort_node_order(b32 left, b32 right)
{
        return string_compare(tsort_nodes[left].name, tsort_nodes[right].name);
}

/* The input was split in place, so the resident hash-and-length primitive can
   index each name in one pass.  A load factor below one half bounds misses
   without introducing a second tree solely for name lookup. */
static b32 tsort_node_for(string_address name)
{
        positive2 named = string_hash_33_length(name);
        positive slot = named.x & (tsort_bucket_count - 1);

        while (tsort_buckets[slot] >= 0)
        {
                b32 have = tsort_buckets[slot];

                if (tsort_nodes[have].hash == named.x &&
                    !string_compare(tsort_nodes[have].name, name))
                        return have;

                slot = (slot + 1) & (tsort_bucket_count - 1);
        }

        b32 made = (b32)tsort_node_count++;
        tsort_nodes[made] = (tsort_node){
            .name = name,
            .hash = named.x,
            .predecessors = 0,
            .top = -1,
            .queue = -1,
            .printed = false,
        };
        tsort_buckets[slot] = made;
        return made;
}

static fn tsort_queue_add(b32 node, b32 address_to head, b32 address_to tail)
{
        if (address_to head < 0)
                address_to head = node;
        else
                tsort_nodes[address_to tail].queue = node;

        address_to tail = node;
}

/* GNU's loop walk is deliberately deterministic rather than canonical: it
   visits names lexically but each name's successors in reverse input order.
   Reusing queue as the backwards trail reproduces that choice, including the
   diagnostic, while removing one relation lets the ordinary sort continue. */
static bool tsort_break_cycle(b32 address_to order, b32 address_to loop)
{
        for (positive at = 0; at < tsort_node_count; at++)
        {
                b32 node = order[at];

                if (!tsort_nodes[node].predecessors)
                        continue;

                if (address_to loop < 0)
                {
                        address_to loop = node;
                        continue;
                }

                b32 address_to link = address_of tsort_nodes[node].top;

                while (address_to link >= 0)
                {
                        tsort_edge address_to edge = tsort_edges + address_to link;

                        if (edge->successor != address_to loop)
                        {
                                link = address_of edge->next;
                                continue;
                        }

                        if (tsort_nodes[node].queue < 0)
                        {
                                tsort_nodes[node].queue = address_to loop;
                                address_to loop = node;
                                break;
                        }

                        while (address_to loop >= 0)
                        {
                                b32 here = address_to loop;
                                b32 next = tsort_nodes[here].queue;

                                text_error(null, tsort_nodes[here].name);

                                if (here == node)
                                {
                                        tsort_nodes[edge->successor].predecessors--;
                                        address_to link = edge->next;
                                        break;
                                }

                                tsort_nodes[here].queue = -1;
                                address_to loop = next;
                        }

                        while (address_to loop >= 0)
                        {
                                b32 next = tsort_nodes[address_to loop].queue;
                                tsort_nodes[address_to loop].queue = -1;
                                address_to loop = next;
                        }

                        return true;
                }
        }

        return false;
}

static b32 tools_tsort()
{
        file_taking taking = {
            .program = (string_address) "tsort",
            .allowed = (string_address) "",
            .valued = (string_address) "",
        };

        text_begin("tsort");
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(1);

        positive arguments = (positive)program_argument_count() - taking.first;

        if (arguments > 1)
                return text_refuse(program_argument((b32)taking.first + 1),
                                   "extra operand", 1);

        string_address path = arguments
                                  ? program_argument((b32)taking.first)
                                  : (string_address) "-";
        bipolar input = 0;
        bool close_input = false;

        if (!string_equals(path, "-"))
        {
                do
                        input = system_open_at(AT_FDCWD, path,
                                               FILE_READ | O_CLOEXEC);
                while (input == TSORT_ERROR_INTERRUPTED);

                if (input < 0)
                        return text_refuse(path, file_reason(input), 1);

                close_input = true;
        }

        positive length;
        bool read_failed;
        p8 address_to bytes = text_arena_read_all(
            (positive)input, TEXT_READ_MAX, address_of length,
            address_of read_failed);

        if (close_input)
                system_close(input);

        if (!bytes)
        {
                if (read_failed)
                        text_error(path, "Read error");
                return text_done(1);
        }

        positive tokens = 0;
        bool inside = false;

        for (positive at = 0; at < length; at++)
        {
                p8 value = bytes[at];

                if (!value)
                        return text_refuse(path, "input contains a NUL byte", 1);

                if (value == ' ' || value == '\t' || value == '\n')
                {
                        bytes[at] = end;
                        inside = false;
                }
                else if (!inside)
                {
                        tokens++;
                        inside = true;
                }
        }

        if (tokens & 1)
                return text_refuse(path,
                                   "input contains an odd number of tokens", 1);
        if (!tokens)
                return text_done(0);

        /* Every relation can introduce two nodes.  These checks make every
           subsequent product and signed graph index representable before an
           arena request has a chance to wrap it. */
        if (tokens > b32_max ||
            tokens > positive_max / sizeof(tsort_node) ||
            tokens / 2 > positive_max / sizeof(tsort_edge))
                return text_refuse(path, "input too large", 1);

        tsort_bucket_count = 8;

        while (tsort_bucket_count < tokens * 2)
        {
                if (tsort_bucket_count > positive_max / 2)
                        return text_refuse(path, "input too large", 1);
                tsort_bucket_count <<= 1;
        }

        if (tsort_bucket_count > positive_max / sizeof(b32))
                return text_refuse(path, "input too large", 1);

        positive node_bytes = tokens * sizeof(tsort_node);
        positive edge_bytes = (tokens / 2) * sizeof(tsort_edge);
        positive bucket_bytes = tsort_bucket_count * sizeof(b32);

        if (node_bytes > positive_max - edge_bytes ||
            node_bytes + edge_bytes > positive_max - bucket_bytes)
                return text_refuse(path, "input too large", 1);

        p8 address_to graph = (p8 address_to)text_arena_take(
            node_bytes + edge_bytes + bucket_bytes);

        if (!graph)
                return text_done(1);

        tsort_nodes = (tsort_node address_to)graph;
        tsort_edges = (tsort_edge address_to)(graph + node_bytes);
        tsort_buckets = (b32 address_to)(graph + node_bytes + edge_bytes);

        memory_fill(tsort_buckets, (b8)-1,
                    tsort_bucket_count * sizeof(b32));
        tsort_node_count = 0;

        positive relation = 0;
        b32 before = -1;
        positive at = 0;

        while (at < length)
        {
                while (at < length && !bytes[at])
                        at++;

                if (at == length)
                        break;

                string_address name = bytes + at;
                b32 node = tsort_node_for(name);
                at += string_length(name);

                if (before < 0)
                {
                        before = node;
                        continue;
                }

                if (before != node)
                {
                        tsort_edges[relation] = (tsort_edge){
                            .successor = node,
                            .next = tsort_nodes[before].top,
                        };
                        tsort_nodes[before].top = (b32)relation++;
                        tsort_nodes[node].predecessors++;
                }

                before = -1;
        }

        if (tsort_node_count > positive_max / (2 * sizeof(b32)))
                return text_refuse(path, "input too large", 1);

        b32 address_to order = (b32 address_to)text_arena_take(
            tsort_node_count * 2 * sizeof(b32));
        b32 address_to spare = order
                                   ? order + tsort_node_count
                                   : null;

        if (!order)
                return text_done(1);

        for (positive i = 0; i < tsort_node_count; i++)
                order[i] = (b32)i;

        order = array_merge_sort(order, spare, tsort_node_count,
                                 tsort_node_order);

        positive left = tsort_node_count;
        b32 head = -1;
        b32 tail = -1;
        bool acyclic = true;

        while (left)
        {
                for (positive i = 0; i < tsort_node_count; i++)
                {
                        b32 node = order[i];

                        if (!tsort_nodes[node].predecessors &&
                            !tsort_nodes[node].printed)
                                tsort_queue_add(node, address_of head,
                                                address_of tail);
                }

                while (head >= 0)
                {
                        b32 node = head;
                        tsort_node address_to here = tsort_nodes + node;

                        text_put_string(here->name);
                        text_put_character('\n');
                        here->printed = true;
                        left--;

                        for (b32 edge = here->top; edge >= 0;
                             edge = tsort_edges[edge].next)
                        {
                                b32 successor = tsort_edges[edge].successor;

                                tsort_nodes[successor].predecessors--;

                                if (!tsort_nodes[successor].predecessors)
                                        tsort_queue_add(successor,
                                                        address_of head,
                                                        address_of tail);
                        }

                        head = here->queue;
                }

                if (left)
                {
                        text_error(path, "input contains a loop:");
                        acyclic = false;
                        b32 loop = -1;

                        do
                        {
                                tsort_break_cycle(order, address_of loop);
                        } while (loop >= 0);
                }
        }

        return text_done(acyclic ? 0 : 1);
}

// numfmt ----------------------------------------------------

/*
        Decimal input stays decimal here too.  numfmt needs multiplication
        and division by user units in addition to seq's power-of-ten scale,
        so the one extra representation is a reduced unsigned rational.  Its
        numerator and denominator are each one native word; inputs whose
        exact reduced form exceeds that bound are rejected instead of being
        rounded through binary floating point.  The source coefficient is
        consequently bounded by signed 64-bit and at most eighteen decimal
        places, the same checked floor seq already owns.
*/
enum
{
        NUMFMT_SCALE_NONE,
        NUMFMT_SCALE_AUTO,
        NUMFMT_SCALE_SI,
        NUMFMT_SCALE_IEC,
        NUMFMT_SCALE_IEC_I,
};

enum
{
        NUMFMT_ROUND_FROM_ZERO,
        NUMFMT_ROUND_UP,
        NUMFMT_ROUND_DOWN,
        NUMFMT_ROUND_TO_ZERO,
        NUMFMT_ROUND_NEAREST,
};

enum
{
        NUMFMT_INVALID_ABORT,
        NUMFMT_INVALID_FAIL,
        NUMFMT_INVALID_WARN,
        NUMFMT_INVALID_IGNORE,
};

typedef struct
{
        string_address text;
        positive directive;
        positive after;
        positive width;
        positive precision;
        bool has_precision;
        bool left;
        bool zero;
        bool grouping;
} numfmt_format;

typedef struct
{
        p8 from;
        p8 to;
        p8 rounding;
        p8 invalid;
        positive from_unit;
        positive to_unit;
        bipolar padding;
        positive header;
        p8 field_delimiter;
        bool delimiter_given;
        bool grouping;
        bool debug;
        string_address suffix;
        string_address unit_separator;
        numfmt_format format;
        bool have_format;
        bool failed;
        bool stop;
} numfmt_options;

static numfmt_options numfmt;

static const file_long numfmt_longs[] = {
    {(string_address) "debug", 'D'},
    {(string_address) "delimiter", 'd'},
    {(string_address) "field", 'f'},
    {(string_address) "format", 'm'},
    {(string_address) "from", 'r'},
    {(string_address) "from-unit", 'R'},
    {(string_address) "grouping", 'g'},
    {(string_address) "header", 'h'},
    {(string_address) "invalid", 'i'},
    {(string_address) "padding", 'p'},
    {(string_address) "round", 'u'},
    {(string_address) "suffix", 's'},
    {(string_address) "unit-separator", 'S'},
    {(string_address) "to", 't'},
    {(string_address) "to-unit", 'T'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

static positive numfmt_gcd(positive left, positive right)
{
        while (right)
        {
                positive next = left % right;
                left = right;
                right = next;
        }

        return left;
}

static bool numfmt_scale_name(string_address name, bool input, p8 address_to scale)
{
        if (string_equals(name, "none"))
                address_to scale = NUMFMT_SCALE_NONE;
        else if (input && string_equals(name, "auto"))
                address_to scale = NUMFMT_SCALE_AUTO;
        else if (string_equals(name, "si"))
                address_to scale = NUMFMT_SCALE_SI;
        else if (string_equals(name, "iec"))
                address_to scale = NUMFMT_SCALE_IEC;
        else if (string_equals(name, "iec-i"))
                address_to scale = NUMFMT_SCALE_IEC_I;
        else
                return false;

        return true;
}

static bool numfmt_power_letter(p8 letter, positive address_to power)
{
        static const p8 names[] = "kMGTPEZYRQ";

        if (letter == 'K')
                letter = 'k';

        for (positive at = 0; names[at]; at++)
                if (names[at] == letter)
                {
                        address_to power = at + 1;
                        return true;
                }

        return false;
}

/* K and Ki are accepted in unit options without a leading one.  Repeating
   the base keeps overflow checked at the place it occurs and avoids a second
   scaled-number parser. */
static bool numfmt_unit(string_address text, positive address_to unit)
{
        positive length = string_length(text);
        positive power = 0;
        positive base = 1000;
        positive digits = length;

        if (length >= 2 && text[length - 1] == 'i' &&
            numfmt_power_letter(text[length - 2], address_of power))
        {
                base = 1024;
                digits -= 2;
        }
        else if (length &&
                 numfmt_power_letter(text[length - 1], address_of power))
                digits--;

        positive value = 1;

        if (digits)
        {
                value = 0;

                for (positive at = 0; at < digits; at++)
                {
                        if (!byte_is_digit(text[at]))
                                return false;

                        positive digit = (positive)(text[at] - '0');

                        if (value > (positive_max - digit) / 10)
                                return false;

                        value = value * 10 + digit;
                }
        }
        else if (!power)
                return false;

        if (!value)
                return false;

        while (power--)
        {
                if (value > positive_max / base)
                        return false;

                value *= base;
        }

        address_to unit = value;
        return true;
}

static bool numfmt_signed_option(string_address text, bipolar address_to value)
{
        positive at = 0;
        bool minus = false;

        if (text[at] == '-' || text[at] == '+')
        {
                minus = text[at] == '-';
                at++;
        }

        if (!text[at])
                return false;

        positive magnitude = 0;
        positive limit = minus ? (positive)bipolar_max + 1
                               : (positive)bipolar_max;

        for (; text[at]; at++)
        {
                if (!byte_is_digit(text[at]))
                        return false;

                positive digit = (positive)(text[at] - '0');

                if (magnitude > (limit - digit) / 10)
                        return false;

                magnitude = magnitude * 10 + digit;
        }

        address_to value = bipolar_from_magnitude(magnitude, minus);
        return true;
}

/* numfmt's format deliberately has a narrower grammar than printf: one %f,
   optional zero/group/left flags, width and an optional decimal precision.
   No precision means the ordinary human-format precision, not printf's six. */
static bool numfmt_format_read(string_address text,
                               numfmt_format address_to format)
{
        bool found = false;
        memory_fill(format, 0, sizeof(*format));
        format->text = text;

        for (positive at = 0; text[at]; at++)
        {
                if (text[at] != '%')
                        continue;

                if (text[at + 1] == '%')
                        /* GNU numfmt copies %% literally and can discard the
                           byte after it while finding the directive.  That
                           is not printf semantics, so refuse this uncommon
                           spelling instead of reproducing the corruption. */
                        return false;

                if (found)
                        return false;

                found = true;
                format->directive = at++;

                while (text[at] == '0' || text[at] == '\'' ||
                       text[at] == '-')
                {
                        if (text[at] == '0')
                                format->zero = true;
                        else if (text[at] == '\'')
                                format->grouping = true;
                        else
                                format->left = true;
                        at++;
                }

                while (byte_is_digit(text[at]))
                {
                        positive digit = (positive)(text[at++] - '0');

                        if (format->width > (TEXT_LINE_MAX - digit) / 10)
                                return false;

                        format->width = format->width * 10 + digit;
                }

                if (text[at] == '.')
                {
                        format->has_precision = true;
                        at++;

                        if (!byte_is_digit(text[at]))
                                return false;

                        while (byte_is_digit(text[at]))
                        {
                                positive digit = (positive)(text[at++] - '0');

                                if (format->precision > 18)
                                        return false;

                                format->precision = format->precision * 10 + digit;
                        }

                        if (format->precision > 18)
                                return false;
                }

                if (text[at] != 'f')
                        return false;

                format->after = at + 1;
        }

        return found;
}

static bool numfmt_span_ends(p8 address_to bytes, positive length,
                             string_address suffix)
{
        positive suffix_length = suffix ? string_length(suffix) : 0;

        return suffix_length && length >= suffix_length &&
               !memory_compare(bytes + length - suffix_length, suffix,
                               suffix_length);
}

/* Normalize only enough to let seq's checked parser own the value.  Leading
   integral zeroes do not consume its coefficient budget, while the original
   fractional width remains visible in `shown`. */
static bool numfmt_decimal(p8 address_to bytes, positive length,
                           seq_decimal address_to number)
{
        positive at = 0;
        bool minus = false;

        if (at < length && bytes[at] == '-')
        {
                minus = true;
                at++;
        }
        else if (at < length && bytes[at] == '+')
                return false;

        positive point = positive_max;
        positive digits = 0;
        positive fractional = 0;

        for (positive scan = at; scan < length; scan++)
                if (bytes[scan] == '.')
                {
                        if (point != positive_max)
                                return false;
                        point = scan;
                }
                else if (byte_is_digit(bytes[scan]))
                {
                        digits++;
                        if (point != positive_max)
                                fractional++;
                }
                else
                        return false;

        if (!digits || (point != positive_max &&
                        (point + 1 == length || fractional > 18)))
                return false;

        positive integral_end = point == positive_max ? length : point;
        positive zeros = 0;

        while (at + zeros < integral_end && bytes[at + zeros] == '0')
                zeros++;

        positive integral = integral_end - at - zeros;
        p8 normalized[48];
        positive made = 0;

        if (minus)
                normalized[made++] = '-';

        if (!integral)
                normalized[made++] = '0';
        else
        {
                if (integral > 20)
                        return false;
                memory_copy(normalized + made, bytes + at + zeros, integral);
                made += integral;
        }

        if (point != positive_max)
        {
                normalized[made++] = '.';
                memory_copy(normalized + made, bytes + point + 1, fractional);
                made += fractional;
        }

        normalized[made] = end;
        return seq_decimal_number(normalized, number);
}

static bool numfmt_ratio(seq_decimal address_to number, positive base,
                         positive power, positive address_to numerator,
                         positive address_to denominator)
{
        positive top[13];
        positive bottom[2];
        positive tops = 0;
        positive bottoms = 0;
        positive magnitude = (positive)number->coefficient;

        if (number->coefficient < 0)
                magnitude = (positive)0 - magnitude;

        if (!magnitude)
        {
                address_to numerator = 0;
                address_to denominator = 1;
                return true;
        }

        top[tops++] = magnitude;
        top[tops++] = numfmt.from_unit;

        while (power--)
                top[tops++] = base;

        if (number->scale)
                bottom[bottoms++] = seq_power_ten(number->scale);
        bottom[bottoms++] = numfmt.to_unit;

        for (positive b = 0; b < bottoms; b++)
                for (positive t = 0; t < tops; t++)
                {
                        positive common = numfmt_gcd(top[t], bottom[b]);
                        top[t] /= common;
                        bottom[b] /= common;
                }

        positive n = 1;
        positive d = 1;

        for (positive at = 0; at < tops; at++)
        {
                if (n > positive_max / top[at])
                        return false;
                n *= top[at];
        }

        for (positive at = 0; at < bottoms; at++)
        {
                if (d > positive_max / bottom[at])
                        return false;
                d *= bottom[at];
        }

        address_to numerator = n;
        address_to denominator = d;
        return true;
}

/* floor(10 * remainder / divisor), without overflowing a native word.  The
   threshold is k*d/10 rounded up; the new remainder uses the already
   available double-width multiply but never requests double-width division. */
static positive numfmt_decimal_digit(positive remainder, positive divisor,
                                     positive address_to next)
{
        positive quotient = divisor / 10;
        positive tail = divisor % 10;
        positive low = 0;
        positive high = 10;

        while (low + 1 < high)
        {
                positive middle = (low + high) / 2;
                positive threshold = middle * quotient +
                                     (middle * tail + 9) / 10;

                if (remainder >= threshold)
                        low = middle;
                else
                        high = middle;
        }

        p128 changed = (p128)remainder * 10 - (p128)low * divisor;
        address_to next = (positive)changed;
        return low;
}

static bool numfmt_round(positive numerator, positive denominator,
                         positive digits, bool negative,
                         positive address_to whole,
                         positive address_to fraction)
{
        positive integer = numerator / denominator;
        positive remainder = numerator % denominator;
        positive decimals = 0;

        for (positive at = 0; at < digits; at++)
        {
                positive digit = numfmt_decimal_digit(remainder, denominator,
                                                       address_of remainder);
                decimals = decimals * 10 + digit;
        }

        bool increase = false;

        if (remainder)
                switch (numfmt.rounding)
                {
                case NUMFMT_ROUND_FROM_ZERO: increase = true; break;
                case NUMFMT_ROUND_UP: increase = !negative; break;
                case NUMFMT_ROUND_DOWN: increase = negative; break;
                case NUMFMT_ROUND_TO_ZERO: break;
                case NUMFMT_ROUND_NEAREST:
                        increase = remainder >= denominator / 2 +
                                                   (denominator & 1);
                        break;
                }

        if (increase)
        {
                positive scale = seq_power_ten(digits);
                decimals++;

                if (decimals == scale)
                {
                        decimals = 0;
                        if (integer == positive_max)
                                return false;
                        integer++;
                }
        }

        address_to whole = integer;
        address_to fraction = decimals;
        return true;
}

static fn numfmt_put_number(positive whole, positive fraction,
                            positive stored_precision,
                            positive shown_precision, bool negative,
                            p8 address_to bytes, positive address_to length)
{
        positive used = 0;

        if (negative)
                bytes[used++] = '-';

        used += positive_into_string(bytes + used, whole);

        if (shown_precision)
        {
                bytes[used++] = '.';

                if (stored_precision)
                {
                        positive missing = stored_precision -
                                           positive_digits(fraction);
                        if (!fraction)
                                missing = stored_precision - 1;
                        memory_fill(bytes + used, '0', missing);
                        used += missing;
                        used += positive_into_string(bytes + used, fraction);
                }

                if (shown_precision > stored_precision)
                {
                        memory_fill(bytes + used, '0',
                                    shown_precision - stored_precision);
                        used += shown_precision - stored_precision;
                }
        }

        address_to length = used;
}

static fn numfmt_body_out(p8 address_to number, positive number_length,
                          p8 address_to unit, positive unit_length,
                          positive automatic_width)
{
        positive separator_length = unit_length && numfmt.unit_separator
                                        ? string_length(numfmt.unit_separator)
                                        : 0;
        positive suffix_length = numfmt.suffix
                                     ? string_length(numfmt.suffix) : 0;
        positive body = number_length + separator_length + unit_length +
                        suffix_length;
        positive width = 0;
        bool left = false;
        positive zero_padding = 0;

        if (numfmt.have_format)
        {
                seq_format_literal(text_put, numfmt.format.text,
                                   numfmt.format.directive);

                if (numfmt.format.zero && !numfmt.format.left)
                {
                        if (numfmt.format.width > number_length)
                                zero_padding = numfmt.format.width -
                                               number_length;

                        /* Zero width belongs to the number conversion and
                           can coexist with an outer --padding. */
                        if (numfmt.padding)
                        {
                                left = numfmt.padding < 0;
                                width = left
                                            ? (positive)0 -
                                                  (positive)numfmt.padding
                                            : (positive)numfmt.padding;
                        }
                }
                else if (numfmt.format.width)
                {
                        width = numfmt.format.width;
                        left = numfmt.format.left;
                }
                else if (numfmt.padding)
                {
                        left = numfmt.padding < 0;
                        width = left ? (positive)0 - (positive)numfmt.padding
                                     : (positive)numfmt.padding;
                }
        }
        else if (numfmt.padding)
        {
                left = numfmt.padding < 0;
                width = left ? (positive)0 - (positive)numfmt.padding
                             : (positive)numfmt.padding;
        }
        else
                width = automatic_width;

        body += zero_padding;
        positive padding = width > body ? width - body : 0;

        if (!left)
                writer_fill(text_put, padding, ' ');

        if (zero_padding && number_length && number[0] == '-')
        {
                text_put(number, 1);
                writer_fill(text_put, zero_padding, '0');
                text_put(number + 1, number_length - 1);
        }
        else
        {
                if (zero_padding)
                        writer_fill(text_put, zero_padding, '0');
                text_put(number, number_length);
        }

        if (separator_length)
                text_put_string(numfmt.unit_separator);
        if (unit_length)
                text_put(unit, unit_length);
        if (suffix_length)
                text_put_string(numfmt.suffix);
        if (left)
                writer_fill(text_put, padding, ' ');

        if (numfmt.have_format)
        {
                string_address after = numfmt.format.text + numfmt.format.after;
                seq_format_literal(text_put, after, string_length(after));
        }
}

static fn numfmt_invalid_value(p8 address_to bytes, positive length)
{
        if (numfmt.invalid == NUMFMT_INVALID_ABORT ||
            numfmt.invalid == NUMFMT_INVALID_FAIL ||
            numfmt.invalid == NUMFMT_INVALID_WARN)
        {
                p8 shown[128];
                positive take = min(length, sizeof(shown) - 1);
                memory_copy(shown, bytes, take);
                shown[take] = end;
                text_error(shown, "invalid number");
        }

        if (numfmt.invalid == NUMFMT_INVALID_ABORT ||
            numfmt.invalid == NUMFMT_INVALID_FAIL)
                numfmt.failed = true;
        if (numfmt.invalid == NUMFMT_INVALID_ABORT)
                numfmt.stop = true;
}

static bool numfmt_convert(p8 address_to bytes, positive length,
                           positive automatic_width)
{
        positive numeric_length = length;

        if (numfmt_span_ends(bytes, numeric_length, numfmt.suffix))
                numeric_length -= string_length(numfmt.suffix);

        positive power = 0;
        positive base = 1;
        positive suffix_bytes = 0;

        if (numfmt.from != NUMFMT_SCALE_NONE && numeric_length)
        {
                bool has_i = numeric_length >= 2 &&
                             bytes[numeric_length - 1] == 'i';
                positive candidate = 0;

                if (has_i &&
                    numfmt_power_letter(bytes[numeric_length - 2],
                                        address_of candidate) &&
                    (numfmt.from == NUMFMT_SCALE_AUTO ||
                     numfmt.from == NUMFMT_SCALE_IEC_I))
                {
                        power = candidate;
                        base = 1024;
                        suffix_bytes = 2;
                }
                else if (!has_i &&
                         numfmt_power_letter(bytes[numeric_length - 1],
                                             address_of candidate) &&
                         numfmt.from != NUMFMT_SCALE_IEC_I)
                {
                        power = candidate;
                        base = numfmt.from == NUMFMT_SCALE_IEC ? 1024 : 1000;
                        suffix_bytes = 1;
                }
        }

        numeric_length -= suffix_bytes;

        if (numfmt_span_ends(bytes, numeric_length, numfmt.unit_separator))
                numeric_length -= string_length(numfmt.unit_separator);

        seq_decimal number;
        positive numerator;
        positive denominator;

        if (!numfmt_decimal(bytes, numeric_length, address_of number) ||
            !numfmt_ratio(address_of number, base, power,
                          address_of numerator, address_of denominator))
        {
                numfmt_invalid_value(bytes, length);
                if (!numfmt.stop)
                        text_put(bytes, length);
                return false;
        }

        if (suffix_bytes)
                number.shown = 0;

        bool negative = number.coefficient < 0 && numerator;
        positive output_power = 0;
        positive output_base = numfmt.to == NUMFMT_SCALE_SI ? 1000 : 1024;

        if (numfmt.to != NUMFMT_SCALE_NONE)
                while (output_power < 10 &&
                       numerator / output_base >= denominator)
                {
                        denominator *= output_base;
                        output_power++;
                }

        positive print_precision;

        if (numfmt.have_format && numfmt.format.has_precision)
                print_precision = numfmt.format.precision;
        else if (numfmt.to == NUMFMT_SCALE_NONE)
                print_precision = number.shown;
        else
                print_precision = output_power &&
                                          numerator / denominator < 10
                                      ? 1 : 0;

        positive round_precision = print_precision;

        if (numfmt.to != NUMFMT_SCALE_NONE)
        {
                if (numfmt.have_format && numfmt.format.has_precision)
                        round_precision = min(round_precision,
                                              output_power * 3);
                else
                        /* GNU carries one guard decimal below ten even when
                           no suffix is needed.  The final integer rendering
                           then uses nearest-even, which is why 0.5 is 0 but
                           1.5 is 2. */
                        round_precision = numerator / denominator < 10 ? 1 : 0;
        }

        positive whole;
        positive fraction;

        if (!numfmt_round(numerator, denominator, round_precision, negative,
                          address_of whole, address_of fraction))
        {
                numfmt_invalid_value(bytes, length);
                if (!numfmt.stop)
                        text_put(bytes, length);
                return false;
        }

        if (numfmt.to != NUMFMT_SCALE_NONE && whole == output_base &&
            !fraction && output_power < 10)
        {
                whole = 1;
                output_power++;
        }

        /* Automatic precision is chosen again after rounding.  9999 SI is
           rounded with one decimal digit while it is 9.999k, but is printed
           as 10k; 1000 remains 1.0k. */
        if (numfmt.to != NUMFMT_SCALE_NONE &&
            !(numfmt.have_format && numfmt.format.has_precision))
                print_precision = output_power && whole < 10 ? 1 : 0;

        /* snprintf supplies a final nearest-even rounding when the automatic
           display has fewer places than the guarded value above.  Do that
           directly in decimal so libc and binary floating point stay out. */
        if (print_precision < round_precision)
        {
                positive divisor = seq_power_ten(round_precision -
                                                 print_precision);
                positive kept = fraction / divisor;
                positive dropped = fraction % divisor;
                positive half = divisor / 2;

                positive last = print_precision ? kept : whole;

                if (dropped > half || (dropped == half && (last & 1)))
                        kept++;

                positive display_scale = seq_power_ten(print_precision);
                if (kept == display_scale)
                {
                        kept = 0;
                        whole++;
                }

                fraction = kept;
                round_precision = print_precision;
        }

        /* Match the exact decimal floor GNU promises for unscaled output:
           a base-10 exponent plus requested precision above LDBL_DIG cannot
           be printed reliably there.  Our parser is exact, but accepting a
           wider surface would make portable scripts disagree on failure. */
        if (numfmt.to == NUMFMT_SCALE_NONE &&
            positive_digits(whole) + print_precision > 19)
        {
                numfmt_invalid_value(bytes, length);
                if (!numfmt.stop)
                        text_put(bytes, length);
                return false;
        }

        p8 number_text[64];
        positive number_length;
        numfmt_put_number(whole, fraction, round_precision, print_precision,
                          negative, number_text, address_of number_length);

        p8 unit[2];
        positive unit_length = 0;

        if (output_power)
        {
                static const p8 powers[] = "kMGTPEZYRQ";
                unit[unit_length++] = powers[output_power - 1];

                if (output_power == 1)
                        unit[0] = numfmt.to == NUMFMT_SCALE_SI ? 'k' : 'K';
                if (numfmt.to == NUMFMT_SCALE_IEC_I)
                        unit[unit_length++] = 'i';
        }

        numfmt_body_out(number_text, number_length, unit, unit_length,
                        automatic_width);
        return true;
}

static fn numfmt_record(p8 address_to bytes, positive length)
{
        if (numfmt.delimiter_given)
        {
                positive start = 0;
                positive field = 1;

                for (positive at = 0; at <= length; at++)
                        if (at == length || bytes[at] == numfmt.field_delimiter)
                        {
                                positive size = at - start;

                                if (text_list_has(field))
                                        numfmt_convert(bytes + start, size, 0);
                                else
                                        text_put(bytes + start, size);

                                if (numfmt.stop)
                                        return;

                                if (at < length)
                                        text_put_character(numfmt.field_delimiter);
                                start = at + 1;
                                field++;
                        }

                return;
        }

        positive at = 0;
        positive field = 0;

        while (at < length)
        {
                if (bytes[at] == ' ' || bytes[at] == '\t')
                {
                        text_put_character(' ');
                        at++;
                        continue;
                }

                positive start = at;

                while (at < length && bytes[at] != ' ' && bytes[at] != '\t')
                        at++;

                positive size = at - start;
                field++;

                if (text_list_has(field))
                        numfmt_convert(bytes + start, size,
                                       !numfmt.padding && !numfmt.have_format
                                           ? size : 0);
                else
                        text_put(bytes + start, size);

                if (numfmt.stop)
                        return;
        }
}

static b32 tools_numfmt()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "numfmt",
            .allowed = (string_address) "dz",
            .valued = (string_address) "dfmpriRsStuT",
            .long_optional = (string_address) "h",
            .longs = numfmt_longs,
            .operand = file_operand,
        };

        text_begin("numfmt");
        numfmt = (numfmt_options){
            .from = NUMFMT_SCALE_NONE,
            .to = NUMFMT_SCALE_NONE,
            .rounding = NUMFMT_ROUND_FROM_ZERO,
            .invalid = NUMFMT_INVALID_ABORT,
            .from_unit = 1,
            .to_unit = 1,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);

        positive flags = taking.flags;
        numfmt.debug = (flags & FILE_FLAG('D')) != 0;
        numfmt.grouping = (flags & FILE_FLAG('g')) != 0;
        numfmt.suffix = file_option_value(address_of taking, 's');
        numfmt.unit_separator = file_option_value(address_of taking, 'S');

        string_address value = file_option_value(address_of taking, 'r');
        if (value && !numfmt_scale_name(value, true, address_of numfmt.from))
                return text_refuse(value, "invalid --from value", 1);

        value = file_option_value(address_of taking, 't');
        if (value && !numfmt_scale_name(value, false, address_of numfmt.to))
                return text_refuse(value, "invalid --to value", 1);

        value = file_option_value(address_of taking, 'R');
        if (value && !numfmt_unit(value, address_of numfmt.from_unit))
                return text_refuse(value, "invalid unit size", 1);

        value = file_option_value(address_of taking, 'T');
        if (value && !numfmt_unit(value, address_of numfmt.to_unit))
                return text_refuse(value, "invalid unit size", 1);

        value = file_option_value(address_of taking, 'p');
        if (value && (!numfmt_signed_option(value, address_of numfmt.padding) ||
                      !numfmt.padding))
                return text_refuse(value, "invalid padding value", 1);

        value = file_option_value(address_of taking, 'u');
        if (value)
        {
                if (string_equals(value, "up"))
                        numfmt.rounding = NUMFMT_ROUND_UP;
                else if (string_equals(value, "down"))
                        numfmt.rounding = NUMFMT_ROUND_DOWN;
                else if (string_equals(value, "from-zero"))
                        numfmt.rounding = NUMFMT_ROUND_FROM_ZERO;
                else if (string_equals(value, "towards-zero"))
                        numfmt.rounding = NUMFMT_ROUND_TO_ZERO;
                else if (string_equals(value, "nearest"))
                        numfmt.rounding = NUMFMT_ROUND_NEAREST;
                else
                        return text_refuse(value, "invalid rounding method", 1);
        }

        value = file_option_value(address_of taking, 'i');
        if (value)
        {
                if (string_equals(value, "abort"))
                        numfmt.invalid = NUMFMT_INVALID_ABORT;
                else if (string_equals(value, "fail"))
                        numfmt.invalid = NUMFMT_INVALID_FAIL;
                else if (string_equals(value, "warn"))
                        numfmt.invalid = NUMFMT_INVALID_WARN;
                else if (string_equals(value, "ignore"))
                        numfmt.invalid = NUMFMT_INVALID_IGNORE;
                else
                        return text_refuse(value, "invalid invalid-mode", 1);
        }

        value = file_option_value(address_of taking, 'm');
        if (value)
        {
                if (!numfmt_format_read(value, address_of numfmt.format))
                        return text_refuse(value,
                                           "format needs exactly one %f conversion",
                                           1);
                numfmt.have_format = true;
        }

        if (numfmt.grouping && numfmt.have_format)
                return text_refuse(null,
                                   "--grouping cannot be combined with --format",
                                   1);
        if (numfmt.grouping && numfmt.to != NUMFMT_SCALE_NONE)
                return text_refuse(null,
                                   "--grouping cannot be combined with --to",
                                   1);

        value = file_option_value(address_of taking, 'd');
        if (flags & FILE_FLAG('d'))
        {
                positive length = string_length(value);

                if (length > 1)
                        return text_refuse(value,
                                           "delimiter must be one byte", 1);
                numfmt.delimiter_given = true;
                numfmt.field_delimiter = length ? value[0] : '\0';
        }

        memory_fill(text_list, 0, sizeof(text_list));
        memory_fill(text_list_begins, 0, sizeof(text_list_begins));
        text_list_open = 0;
        value = file_option_value(address_of taking, 'f');
        if (!value)
                value = (string_address) "1";
        if (!text_list_parse(value))
                return text_refuse(value, "invalid field specification", 1);

        if (flags & FILE_FLAG('h'))
        {
                value = file_option_value(address_of taking, 'h');
                numfmt.header = 1;

                if (value && (!string_digits_exact(value, address_of numfmt.header) ||
                              !numfmt.header))
                        return text_refuse(value, "invalid header value", 1);
        }

        text_delimiter = (flags & FILE_FLAG('z')) ? '\0' : '\n';

        if (file_operand_count)
        {
                for (positive at = 0; at < file_operand_count && !numfmt.stop; at++)
                {
                        string_address word = file_operand_at(at);
                        numfmt_convert(word, string_length(word), 0);
                        if (!numfmt.stop)
                                text_put_character(text_delimiter);
                }
        }
        else if (text_open(null))
        {
                positive records = 0;

                while (!numfmt.stop && text_line_next())
                {
                        if (records++ < numfmt.header)
                                text_put(text_line, text_line_length);
                        else
                                numfmt_record(text_line, text_line_length);

                        if (!numfmt.stop)
                                text_put_character(text_delimiter);
                }

                text_close();
        }

        return text_done(numfmt.failed ? 2 : text_status);
}

// factor ----------------------------------------------------

/*
        This is deliberately a native-word factorizer, not a miniature
        bignum package.  The shared checked decimal floor accepts 0 through
        18446744073709551615 on the 64-bit targets; a longer value is an
        error, never a truncated factorization.  All visible numbers go back
        through the resident positive_to_string writer.

        Odd modular arithmetic uses Montgomery form.  Its reduction needs a
        64x64->128 multiply and shifts, but no double-width division (and
        therefore no compiler runtime hiding in the freestanding binary).
        Deterministic Miller-Rabin bases cover the complete 64-bit interval;
        Brent's Pollard rho uses a deterministic polynomial schedule so it
        does not grow a second RNG beside file_random_state.
*/
typedef struct
{
        positive modulus;
        positive inverse;
        positive one;
        positive square;
} factor_modulus;

static positive factor_add_mod(positive left, positive right,
                               positive modulus)
{
        return left >= modulus - right
                   ? left - (modulus - right)
                   : left + right;
}

static fn factor_modulus_begin(factor_modulus address_to context,
                               positive modulus)
{
        positive inverse = 1;

        /* Newton doubles the correct inverse bits at each step. */
        for (positive at = 0; at < 6; at++)
                inverse *= 2 - modulus * inverse;

        context->modulus = modulus;
        context->inverse = (positive)0 - inverse;

        positive one = 1;
        for (positive at = 0; at < positive_bits; at++)
                one = factor_add_mod(one, one, modulus);
        context->one = one;

        positive square = one;
        for (positive at = 0; at < positive_bits; at++)
                square = factor_add_mod(square, square, modulus);
        context->square = square;
}

static positive factor_multiply(factor_modulus address_to context,
                                positive left, positive right)
{
        p128 product = (p128)left * right;
        positive correction = (positive)product * context->inverse;
        p128 adjusted = (p128)correction * context->modulus;
        p128 reduced = product + adjusted;
        positive answer = (positive)(reduced >> positive_bits);

        /* The mathematical sum can carry out of the 128-bit object.  In
           that case its high word is R+answer and the mandatory subtraction
           of modulus is represented by the same native wrap. */
        if (reduced < product)
                return answer - context->modulus;

        return answer >= context->modulus
                   ? answer - context->modulus : answer;
}

static positive factor_into_montgomery(factor_modulus address_to context,
                                       positive value)
{
        return factor_multiply(context, value % context->modulus,
                               context->square);
}

static positive factor_power(factor_modulus address_to context,
                             positive base, positive exponent)
{
        positive answer = context->one;
        positive value = factor_into_montgomery(context, base);

        while (exponent)
        {
                if (exponent & 1)
                        answer = factor_multiply(context, answer, value);
                exponent >>= 1;

                if (exponent)
                        value = factor_multiply(context, value, value);
        }

        return answer;
}

static positive factor_gcd(positive left, positive right)
{
        while (right)
        {
                positive next = left % right;
                left = right;
                right = next;
        }

        return left;
}

static bool factor_is_prime(positive number)
{
        static const positive bases[] = {
            2, 325, 9375, 28178, 450775, 9780504, 1795265022,
        };

        if (number < 4)
                return number == 2 || number == 3;
        if (!(number & 1))
                return false;

        positive odd = number - 1;
        positive shifts = 0;

        while (!(odd & 1))
        {
                odd >>= 1;
                shifts++;
        }

        factor_modulus context;
        factor_modulus_begin(address_of context, number);
        positive negative_one = number - context.one;

        for (positive at = 0; at < array_count(bases); at++)
        {
                positive base = bases[at] % number;

                if (!base)
                        continue;

                positive witness = factor_power(address_of context, base, odd);

                if (witness == context.one || witness == negative_one)
                        continue;

                bool passed = false;

                for (positive square = 1; square < shifts; square++)
                {
                        witness = factor_multiply(address_of context,
                                                  witness, witness);

                        if (witness == negative_one)
                        {
                                passed = true;
                                break;
                        }

                        if (witness == context.one)
                                return false;
                }

                if (!passed)
                        return false;
        }

        return true;
}

static positive factor_polynomial(factor_modulus address_to context,
                                  positive value, positive constant)
{
        return factor_add_mod(factor_multiply(context, value, value),
                              constant, context->modulus);
}

static positive factor_rho(positive number)
{
        factor_modulus context;
        factor_modulus_begin(address_of context, number);

        /* Different constants and starts are independent deterministic rho
           walks.  A per-walk budget bounds every failure before the next
           polynomial is tried. */
        for (positive attempt = 0; attempt < 32; attempt++)
        {
                positive constant = factor_into_montgomery(
                    address_of context, 1 + attempt * 2);
                positive y = factor_into_montgomery(
                    address_of context, 2 + attempt * 3);
                positive x = 0;
                positive saved = 0;
                positive product = context.one;
                positive divisor = 1;
                positive run = 1;
                positive budget = 1u << 23;

                while (divisor == 1 && budget)
                {
                        x = y;

                        for (positive at = 0; at < run && budget; at++)
                        {
                                y = factor_polynomial(address_of context, y,
                                                      constant);
                                budget--;
                        }

                        positive done = 0;

                        while (done < run && divisor == 1 && budget)
                        {
                                saved = y;
                                positive block = min((positive)128, run - done);
                                product = context.one;

                                for (positive at = 0; at < block && budget; at++)
                                {
                                        y = factor_polynomial(address_of context,
                                                              y, constant);
                                        positive difference = x > y ? x - y
                                                                    : y - x;
                                        product = factor_multiply(
                                            address_of context, product,
                                            difference);
                                        budget--;
                                }

                                divisor = factor_gcd(product, number);
                                done += block;
                        }

                        if (run > ((positive)1 << 22))
                                break;
                        run <<= 1;
                }

                if (divisor > 1 && divisor < number)
                        return divisor;

                if (divisor == number)
                {
                        divisor = 1;
                        while (divisor == 1)
                        {
                                if (!budget)
                                        break;
                                saved = factor_polynomial(address_of context,
                                                          saved, constant);
                                positive difference = x > saved ? x - saved
                                                                : saved - x;
                                divisor = factor_gcd(difference, number);
                                budget--;
                        }
                }

                if (divisor > 1 && divisor < number)
                        return divisor;
        }

        return 0;
}

static bool factor_collect(positive number, positive address_to factors,
                           positive address_to count)
{
        if (number == 1)
                return true;

        if (factor_is_prime(number))
        {
                if (address_to count == positive_bits)
                        return false;
                factors[(address_to count)++] = number;
                return true;
        }

        positive divisor = factor_rho(number);

        return divisor && factor_collect(divisor, factors, count) &&
               factor_collect(number / divisor, factors, count);
}

static bool factor_number(p8 address_to bytes, positive length,
                          bool exponents)
{
        p8 decimal[32];
        positive start = length && bytes[0] == '+' ? 1 : 0;
        positive value;

        if (start == length || length - start >= sizeof(decimal))
                goto invalid;

        memory_copy(decimal, bytes + start, length - start);
        decimal[length - start] = end;

        /* The checked digit floor is shared with shred's native sizes.  Keep
           its cursor so an overflow, sign, point or trailing byte cannot be
           mistaken for a numeric prefix (string_digits_exact intentionally
           has wrapping arithmetic for older callers). */
        string_address after = decimal;
        if (!string_digits_checked(address_of after, 10, address_of value) ||
            string_get(after))
                goto invalid;

        positive_to_string(text_put, value);
        text_put_character(':');

        if (value > 1)
        {
                static const p8 small_primes[] = {
                    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41,
                    43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97,
                };
                positive factors[positive_bits];
                positive count = 0;
                positive remaining = value;

                for (positive at = 0; at < array_count(small_primes); at++)
                {
                        positive prime = small_primes[at];

                        while (!(remaining % prime))
                        {
                                factors[count++] = prime;
                                remaining /= prime;
                        }
                }

                /* A short scalar walk beats setting up seven Montgomery
                   witnesses for the tiny values that dominate shell loops.
                   Bound it at one million: at most 450 odd remainders are
                   tried, while real 32/64-bit work still goes directly to
                   Miller-Rabin and rho. */
                if (remaining < 1000000)
                {
                        for (positive trial = 101;
                             trial <= remaining / trial; trial += 2)
                                while (!(remaining % trial))
                                {
                                        factors[count++] = trial;
                                        remaining /= trial;
                                }

                        if (remaining > 1)
                                factors[count++] = remaining;
                        remaining = 1;
                }

                if (remaining > 1 &&
                    !factor_collect(remaining, factors, address_of count))
                {
                        text_error(decimal, "factorization did not converge");
                        text_put_character('\n');
                        return false;
                }

                for (positive at = 1; at < count; at++)
                {
                        positive held = factors[at];
                        positive before = at;

                        while (before && factors[before - 1] > held)
                        {
                                factors[before] = factors[before - 1];
                                before--;
                        }

                        factors[before] = held;
                }

                for (positive at = 0; at < count;)
                {
                        positive after = at + 1;

                        while (after < count && factors[after] == factors[at])
                                after++;

                        text_put_character(' ');
                        positive_to_string(text_put, factors[at]);

                        if (exponents && after - at > 1)
                        {
                                text_put_character('^');
                                positive_to_string(text_put, after - at);
                        }

                        at = exponents ? after : at + 1;
                }
        }

        text_put_character('\n');
        return true;

invalid:
        {
                p8 shown[64];
                positive take = min(length, sizeof(shown) - 1);
                memory_copy(shown, bytes, take);
                shown[take] = end;
                text_error(shown,
                           "not a valid positive native-word integer");
        }
        return false;
}

static const file_long factor_longs[] = {
    {(string_address) "exponents", 'h'},
    {null, 0},
};

static b32 tools_factor()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "factor",
            .allowed = (string_address) "h",
            .valued = (string_address) "",
            .longs = factor_longs,
            .operand = file_operand,
        };

        text_begin("factor");

        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);

        bool exponents = (taking.flags & FILE_FLAG('h')) != 0;
        bool failed = false;

        if (file_operand_count)
        {
                for (positive at = 0; at < file_operand_count; at++)
                {
                        string_address word = file_operand_at(at);
                        if (!factor_number(word, string_length(word), exponents))
                                failed = true;
                }
        }
        else
        {
                text_reader input;

                if (!text_reader_open(address_of input, null))
                        return text_done(1);

                p8 token[32];
                positive length = 0;
                bool excess = false;

                while (text_reader_fill(address_of input))
                {
                        p8 byte = input.buffer[input.position++];

                        if (byte_is_space(byte))
                        {
                                if (length || excess)
                                {
                                        if (excess ||
                                            !factor_number(token, length,
                                                           exponents))
                                        {
                                                if (excess)
                                                        text_error(null,
                                                                   "integer exceeds native-word ceiling");
                                                failed = true;
                                        }
                                        length = 0;
                                        excess = false;
                                }
                        }
                        else if (length < sizeof(token) - 1)
                                token[length++] = byte;
                        else
                                excess = true;
                }

                if (length || excess)
                {
                        if (excess || !factor_number(token, length, exponents))
                        {
                                if (excess)
                                        text_error(null,
                                                   "integer exceeds native-word ceiling");
                                failed = true;
                        }
                }

                if (input.failed)
                        failed = true;
                text_close_handle(address_of input.opened, input.handle);
        }

        return text_done(failed ? 1 : 0);
}

// UUID identities: uuidgen, uuidparse and mcookie -----------------

/* blkid already owns the canonical UUID byte formatter, shred owns the one
   kernel-seeded random stream, and checksum owns the one MD5/SHA1 backend.
   These three applets join those primitives here instead of bringing in
   libuuid, another PRNG, or software copies of either digest. */
typedef struct
{
        p8 bytes[16];
} tools_uuid;

static bipolar tools_uuid_nibble(p8 byte)
{
        if (byte >= '0' && byte <= '9')
                return byte - '0';
        byte = byte_to_lower(byte);
        return byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : -1;
}

static bool tools_uuid_parse(string_address text,
                             tools_uuid address_to uuid)
{
        if (string_length(text) != 36)
                return false;

        positive from = 0;
        for (positive at = 0; at < sizeof(uuid->bytes); at++)
        {
                if (at == 4 || at == 6 || at == 8 || at == 10)
                {
                        if (text[from++] != '-')
                                return false;
                }

                bipolar high = tools_uuid_nibble(text[from++]);
                bipolar low = tools_uuid_nibble(text[from++]);
                if (high < 0 || low < 0)
                        return false;
                uuid->bytes[at] = (p8)((positive)high << 4 | (positive)low);
        }

        return true;
}

static fn tools_uuid_put(tools_uuid address_to uuid)
{
        p8 text[37];
        storage_uuid_bytes(text, uuid->bytes);
        text_put(text, 36);
}

static fn tools_uuid_random_bytes(file_random_state address_to random,
                                  tools_uuid address_to uuid)
{
        shred_random_fill(random, uuid->bytes, sizeof(uuid->bytes));
}

static fn tools_uuid_version(tools_uuid address_to uuid, p8 version)
{
        uuid->bytes[6] = (p8)((uuid->bytes[6] & 0x0f) | (version << 4));
        uuid->bytes[8] = (p8)((uuid->bytes[8] & 0x3f) | 0x80);
}

static fn tools_uuid_store_16(p8 address_to into, p16 value)
{
        into[0] = (p8)(value >> 8);
        into[1] = (p8)value;
}

static fn tools_uuid_store_32(p8 address_to into, p32 value)
{
        into[0] = (p8)(value >> 24);
        into[1] = (p8)(value >> 16);
        into[2] = (p8)(value >> 8);
        into[3] = (p8)value;
}

static p16 tools_uuid_load_16(p8 address_to from)
{
        return ((p16)from[0] << 8) | from[1];
}

static p32 tools_uuid_load_32(p8 address_to from)
{
        return ((p32)from[0] << 24) | ((p32)from[1] << 16) |
               ((p32)from[2] << 8) | from[3];
}

static p64 tools_uuid_gregorian_now(p64 seconds, p64 nanoseconds)
{
        return seconds * 10000000 + nanoseconds / 100 +
               (p64)0x01b21dd213814000ULL;
}

static fn tools_uuid_time_one(tools_uuid address_to uuid, p64 timestamp,
                              p16 sequence, p8 address_to node)
{
        tools_uuid_store_32(uuid->bytes, (p32)timestamp);
        tools_uuid_store_16(uuid->bytes + 4, (p16)(timestamp >> 32));
        tools_uuid_store_16(uuid->bytes + 6,
                            (p16)((timestamp >> 48) & 0x0fff));
        tools_uuid_store_16(uuid->bytes + 8,
                            (p16)((sequence & 0x3fff) | 0x8000));
        memory_copy(uuid->bytes + 10, node, 6);
        tools_uuid_version(uuid, 1);
}

static fn tools_uuid_time_six(tools_uuid address_to uuid, p64 timestamp,
                              file_random_state address_to random)
{
        tools_uuid_random_bytes(random, uuid);
        tools_uuid_store_32(uuid->bytes, (p32)(timestamp >> 28));
        tools_uuid_store_16(uuid->bytes + 4, (p16)(timestamp >> 12));
        tools_uuid_store_16(uuid->bytes + 6,
                            (p16)(timestamp & 0x0fff));
        tools_uuid_version(uuid, 6);
}

static fn tools_uuid_time_seven(tools_uuid address_to uuid, p64 milliseconds,
                                file_random_state address_to random)
{
        tools_uuid_random_bytes(random, uuid);
        for (positive at = 0; at < 6; at++)
                uuid->bytes[5 - at] = (p8)(milliseconds >> (at * 8));
        tools_uuid_version(uuid, 7);
}

#if defined(LINUX)
static bipolar tools_uuid_name_transform(bool sha1)
{
        const checksum_algorithm address_to algorithm =
            checksum_algorithm_find(sha1 ? (string_address)"sha1sum"
                                         : (string_address)"md5sum");

        return algorithm ? checksum_kernel_open(algorithm) : -ERROR_INVALID;
}

static bool tools_uuid_name(bipolar transform, tools_uuid address_to space,
                            p8 address_to name, positive name_length,
                            bool sha1, tools_uuid address_to uuid)
{
        bipolar operation = checksum_operation_open(transform);
        if (operation < 0)
                return false;

        p8 digest[20];
        bipolar answer = checksum_send_more(operation, space->bytes,
                                             sizeof(space->bytes));
        if (!answer)
                answer = checksum_send_more(operation, name, name_length);
        if (!answer)
                answer = checksum_digest_read(operation, digest,
                                              sha1 ? 20 : 16);
        system_close((positive)operation);

        if (answer < 0)
                return false;

        memory_copy(uuid->bytes, digest, sizeof(uuid->bytes));
        tools_uuid_version(uuid, sha1 ? 5 : 3);
        return true;
}
#endif

static const file_long tools_uuidgen_longs[] = {
    {(string_address)"count", 'C'},
    {(string_address)"hex", 'x'},
    {(string_address)"md5", 'm'},
    {(string_address)"name", 'N'},
    {(string_address)"namespace", 'n'},
    {(string_address)"random", 'r'},
    {(string_address)"sha1", 's'},
    {(string_address)"time", 't'},
    {(string_address)"time-v6", '6'},
    {(string_address)"time-v7", '7'},
    {null, 0},
};

static bool tools_uuidgen_namespace(string_address text,
                                    tools_uuid address_to space)
{
        static const string_address shortcuts[] = {
            (string_address)"@dns", (string_address)"@url",
            (string_address)"@oid", (string_address)"@x500",
        };
        static const string_address uuids[] = {
            (string_address)"6ba7b810-9dad-11d1-80b4-00c04fd430c8",
            (string_address)"6ba7b811-9dad-11d1-80b4-00c04fd430c8",
            (string_address)"6ba7b812-9dad-11d1-80b4-00c04fd430c8",
            (string_address)"6ba7b814-9dad-11d1-80b4-00c04fd430c8",
        };

        for (positive at = 0; at < array_count(shortcuts); at++)
                if (string_equals(text, shortcuts[at]))
                        return tools_uuid_parse(uuids[at], space);

        return tools_uuid_parse(text, space);
}

static bool tools_uuidgen_hex_name(string_address text,
                                   p8 address_to address_to bytes,
                                   positive address_to length)
{
        positive count = string_length(text);
        if (count & 1)
                return false;

        p8 address_to decoded = text_arena_take(count / 2 + 1);
        if (!decoded)
                return false;

        for (positive at = 0; at < count; at += 2)
        {
                bipolar high = tools_uuid_nibble(text[at]);
                bipolar low = tools_uuid_nibble(text[at + 1]);
                if (high < 0 || low < 0)
                        return false;
                decoded[at / 2] = (p8)((positive)high << 4 | (positive)low);
        }

        address_to bytes = decoded;
        address_to length = count / 2;
        return true;
}

static b32 tools_uuidgen()
{
        file_taking taking = {
            .program = (string_address)"uuidgen",
            .allowed = (string_address)"rtmnNsC67x",
            .valued = (string_address)"nNC",
            .longs = tools_uuidgen_longs,
        };

        text_begin("uuidgen");
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(1);

        positive flags = taking.flags;
        string_address namespace = file_option_value(address_of taking, 'n');
        string_address name = file_option_value(address_of taking, 'N');
        string_address count_text = file_option_value(address_of taking, 'C');
        bool md5 = (flags & FILE_FLAG('m')) != 0;
        bool sha1 = (flags & FILE_FLAG('s')) != 0;
        bool hex = (flags & FILE_FLAG('x')) != 0;
        bool named = namespace || name || md5 || sha1;
        positive count = 1;

        if (count_text)
        {
                string_address after = count_text;
                if (!string_digits_checked(address_of after, 10,
                                           address_of count) || string_get(after))
                        return text_refuse(count_text, "invalid count", 1);
        }

        positive modes = ((flags & FILE_FLAG('r')) != 0) +
                         ((flags & FILE_FLAG('t')) != 0) +
                         ((flags & FILE_FLAG('6')) != 0) +
                         ((flags & FILE_FLAG('7')) != 0) + named;

        if (modes > 1)
                return text_refuse(null, "generation modes cannot be combined",
                                   1);

        if (named)
        {
                if (count_text)
                        return text_refuse(null,
                                           "name mode and --count cannot be combined",
                                           1);
                if (!namespace || !name || md5 == sha1)
                        return text_refuse(null,
                                           "name mode needs namespace, name and exactly one digest",
                                           1);

                tools_uuid space;
                if (!tools_uuidgen_namespace(namespace, address_of space))
                        return text_refuse(namespace, "invalid namespace UUID",
                                           1);

                p8 address_to name_bytes = (p8 address_to)name;
                positive name_length = string_length(name);
                if (hex &&
                    !tools_uuidgen_hex_name(name, address_of name_bytes,
                                            address_of name_length))
                        return text_refuse(name, "invalid hexadecimal name", 1);

#if defined(LINUX)
                bipolar transform = tools_uuid_name_transform(sha1);
                if (transform < 0)
                        return text_refuse(null,
                                           "kernel MD5/SHA1 service is unavailable",
                                           1);

                tools_uuid uuid;
                bool made = tools_uuid_name(transform, address_of space,
                                            name_bytes, name_length, sha1,
                                            address_of uuid);
                system_close((positive)transform);
                if (!made)
                        return text_refuse(null,
                                           "kernel UUID digest failed", 1);
                tools_uuid_put(address_of uuid);
                text_put_character('\n');
                return text_done(0);
#else
                return text_refuse(null,
                                   "name UUIDs require the Linux hash service",
                                   1);
#endif
        }

        if (!count)
                return text_done(0);

        file_random_state random;
        if (!file_random_seed(address_of random))
                return text_refuse(null, "kernel random source failed", 1);

        bool time_one = (flags & FILE_FLAG('t')) != 0;
        bool time_six = (flags & FILE_FLAG('6')) != 0;
        bool time_seven = (flags & FILE_FLAG('7')) != 0;
        p64 wall[2] = {0, 0};
        p64 timestamp = 0;
        p64 milliseconds = 0;
        p16 sequence = (p16)file_random_word(address_of random);
        p8 node[6];
        shred_random_fill(address_of random, node, sizeof(node));
        node[0] |= 1;

        if ((time_one || time_six || time_seven) &&
            system_call_2(syscall(clock_gettime), 0,
                          (positive)wall) < 0)
                return text_refuse(null, "realtime clock failed", 1);

        if (time_one || time_six)
                timestamp = tools_uuid_gregorian_now(wall[0], wall[1]);
        else if (time_seven)
                milliseconds = wall[0] * 1000 + wall[1] / 1000000;

        for (positive at = 0; at < count; at++)
        {
                tools_uuid uuid;

                if (time_one)
                        tools_uuid_time_one(address_of uuid, timestamp + at,
                                            sequence, node);
                else if (time_six)
                        tools_uuid_time_six(address_of uuid, timestamp + at,
                                            address_of random);
                else if (time_seven)
                        tools_uuid_time_seven(address_of uuid, milliseconds,
                                              address_of random);
                else
                {
                        tools_uuid_random_bytes(address_of random,
                                                address_of uuid);
                        tools_uuid_version(address_of uuid, 4);
                }

                tools_uuid_put(address_of uuid);
                text_put_character('\n');
        }

        return text_done(0);
}

typedef enum
{
        TOOLS_UUID_COLUMN_UUID,
        TOOLS_UUID_COLUMN_VARIANT,
        TOOLS_UUID_COLUMN_TYPE,
        TOOLS_UUID_COLUMN_TIME,
} tools_uuid_column;

typedef struct
{
        string_address original;
        tools_uuid uuid;
        bool valid;
        string_address variant;
        string_address type;
        p8 time[40];
} tools_uuid_record;

static string_address tools_uuid_variant(tools_uuid address_to uuid)
{
        p8 mark = uuid->bytes[8];
        if (!(mark & 0x80))
                return (string_address)"NCS";
        if ((mark & 0xc0) == 0x80)
                return (string_address)"DCE";
        if ((mark & 0xe0) == 0xc0)
                return (string_address)"Microsoft";
        return (string_address)"other";
}

static bool tools_uuid_all(tools_uuid address_to uuid, p8 value)
{
        for (positive at = 0; at < sizeof(uuid->bytes); at++)
                if (uuid->bytes[at] != value)
                        return false;
        return true;
}

static positive tools_uuid_time_text(p8 address_to into, b64 seconds,
                                     positive microseconds)
{
        b64 year;
        positive month, day, hour, minute, second;
        file_split_moment((b64)seconds, address_of year, address_of month,
                          address_of day, address_of hour, address_of minute,
                          address_of second);

        positive made = positive_into_padded(into, (positive)year, 4, '0');
        into[made++] = '-';
        made += positive_into_padded(into + made, month, 2, '0');
        into[made++] = '-';
        made += positive_into_padded(into + made, day, 2, '0');
        into[made++] = ' ';
        made += positive_into_padded(into + made, hour, 2, '0');
        into[made++] = ':';
        made += positive_into_padded(into + made, minute, 2, '0');
        into[made++] = ':';
        made += positive_into_padded(into + made, second, 2, '0');
        into[made++] = ',';
        made += positive_into_padded(into + made, microseconds, 6, '0');
        memory_copy(into + made, "+00:00", 6);
        made += 6;
        into[made] = end;
        return made;
}

static fn tools_uuid_record_read(string_address text,
                                 tools_uuid_record address_to record)
{
        memory_fill(record, 0, sizeof(*record));
        record->original = text;
        record->valid = tools_uuid_parse(text, address_of record->uuid);

        if (!record->valid)
        {
                record->variant = (string_address)"invalid";
                record->type = (string_address)"invalid";
                memory_copy(record->time, "invalid", 8);
                return;
        }

        record->variant = tools_uuid_variant(address_of record->uuid);
        record->type = (string_address)"";
        if (tools_uuid_all(address_of record->uuid, 0) ||
            tools_uuid_all(address_of record->uuid, 0xff))
                return;

        p8 version = record->uuid.bytes[6] >> 4;
        static const string_address types[9] = {
            (string_address)"", (string_address)"time-based",
            (string_address)"DCE", (string_address)"name-based",
            (string_address)"random", (string_address)"sha1-based",
            (string_address)"time-v6", (string_address)"time-v7",
            (string_address)"vendor",
        };
        record->type = version < array_count(types) && types[version][0]
                           ? types[version] : (string_address)"unknown";

        b64 seconds;
        positive microseconds;
        if (version == 1)
        {
                p64 timestamp = tools_uuid_load_32(record->uuid.bytes) |
                                ((p64)tools_uuid_load_16(
                                     record->uuid.bytes + 4) << 32) |
                                ((p64)(tools_uuid_load_16(
                                      record->uuid.bytes + 6) & 0x0fff) << 48);
                b64 ticks = (b64)timestamp -
                            (b64)0x01b21dd213814000ULL;
                seconds = ticks / 10000000;
                b64 rest = ticks % 10000000;
                if (rest < 0)
                {
                        seconds--;
                        rest += 10000000;
                }
                microseconds = (positive)(rest / 10);
        }
        else if (version == 6)
        {
                p64 timestamp =
                    ((p64)tools_uuid_load_32(record->uuid.bytes) << 28) |
                    ((p64)tools_uuid_load_16(record->uuid.bytes + 4) << 12) |
                    (tools_uuid_load_16(record->uuid.bytes + 6) & 0x0fff);
                b64 ticks = (b64)timestamp -
                            (b64)0x01b21dd213814000ULL;
                seconds = ticks / 10000000;
                b64 rest = ticks % 10000000;
                if (rest < 0)
                {
                        seconds--;
                        rest += 10000000;
                }
                microseconds = (positive)(rest / 10);
        }
        else if (version == 7)
        {
                p64 milliseconds = 0;
                for (positive at = 0; at < 6; at++)
                        milliseconds = (milliseconds << 8) |
                                       record->uuid.bytes[at];
                seconds = milliseconds / 1000;
                microseconds = (positive)(milliseconds % 1000) * 1000;
        }
        else
                return;

        tools_uuid_time_text(record->time, seconds, microseconds);
}

static const string_address tools_uuid_column_names[] = {
    (string_address)"UUID", (string_address)"VARIANT",
    (string_address)"TYPE", (string_address)"TIME",
};

static bool tools_uuid_columns(string_address text, p8 address_to columns,
                               positive address_to count)
{
        positive used = 0;
        while (string_get(text))
        {
                string_address comma = string_first_of(text, ',');
                positive length = comma ? (positive)(comma - text)
                                        : string_length(text);
                bool found = false;

                for (positive at = 0; at < array_count(tools_uuid_column_names);
                     at++)
                        if (string_length(tools_uuid_column_names[at]) == length &&
                            !string_compare_max(tools_uuid_column_names[at], text,
                                                length))
                        {
                                if (used == 32)
                                        return false;
                                columns[used++] = (p8)at;
                                found = true;
                                break;
                        }

                if (!found)
                        return false;
                text += length;
                if (!comma)
                        break;
                text++;
                if (!string_get(text))
                        return false;
        }

        address_to count = used;
        return used != 0;
}

static string_address tools_uuid_cell(tools_uuid_record address_to record,
                                      p8 column)
{
        if (column == TOOLS_UUID_COLUMN_UUID)
                return record->original;
        if (column == TOOLS_UUID_COLUMN_VARIANT)
                return record->variant;
        if (column == TOOLS_UUID_COLUMN_TYPE)
                return record->type;
        return record->time;
}

static fn tools_uuid_json_string(string_address value)
{
        text_put_character('"');
        while (string_get(value))
        {
                p8 byte = string_get(value++);
                if (byte == '"' || byte == '\\')
                {
                        text_put_character('\\');
                        text_put_character(byte);
                }
                else if (byte < ' ')
                {
                        p8 escaped[6] = {'\\', 'u', '0', '0',
                                         storage_hex_digit(byte >> 4, false),
                                         storage_hex_digit(byte & 15, false)};
                        text_put(escaped, sizeof(escaped));
                }
                else
                        text_put_character(byte);
        }
        text_put_character('"');
}

static fn tools_uuid_safe_cell(string_address value)
{
        while (string_get(value))
        {
                p8 byte = string_get(value++);
                if (byte <= ' ' || byte == 0x7f || byte == '\\')
                {
                        p8 escaped[4] = {'\\', 'x',
                                         storage_hex_digit(byte >> 4, false),
                                         storage_hex_digit(byte & 15, false)};
                        text_put(escaped, sizeof(escaped));
                }
                else
                        text_put_character(byte);
        }
}

static const file_long tools_uuidparse_longs[] = {
    {(string_address)"json", 'J'},
    {(string_address)"noheadings", 'n'},
    {(string_address)"output", 'o'},
    {(string_address)"raw", 'r'},
    {null, 0},
};

static b32 tools_uuidparse()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"uuidparse",
            .allowed = (string_address)"Jnro",
            .valued = (string_address)"o",
            .longs = tools_uuidparse_longs,
            .operand = file_operand,
        };

        text_begin("uuidparse");
        if (!file_take(address_of taking) || file_operand_failed)
                return text_done(1);

        bool json = (taking.flags & FILE_FLAG('J')) != 0;
        bool noheadings = (taking.flags & FILE_FLAG('n')) != 0;
        bool raw = (taking.flags & FILE_FLAG('r')) != 0;
        if (json && raw)
                return text_refuse(null,
                                   "--json and --raw cannot be combined", 1);

        p8 columns[32] = {
            TOOLS_UUID_COLUMN_UUID, TOOLS_UUID_COLUMN_VARIANT,
            TOOLS_UUID_COLUMN_TYPE, TOOLS_UUID_COLUMN_TIME,
        };
        positive column_count = 4;
        string_address output = file_option_value(address_of taking, 'o');
        if (output && !tools_uuid_columns(output, columns, address_of column_count))
                return text_refuse(output, "unknown or excessive output column",
                                   1);

        if (json)
        {
                text_put_string("{\n   \"uuids\": [\n");
                for (positive row = 0; row < file_operand_count; row++)
                {
                        tools_uuid_record record;
                        tools_uuid_record_read(file_operand_at(row),
                                               address_of record);
                        text_put_string(row ? ",{\n" : "      {\n");
                        for (positive at = 0; at < column_count; at++)
                        {
                                p8 column = columns[at];
                                text_put_string("         \"");
                                string_address key =
                                    tools_uuid_column_names[column];
                                while (string_get(key))
                                        text_put_character(byte_to_lower(
                                            string_get(key++)));
                                text_put_string("\": ");
                                string_address value =
                                    tools_uuid_cell(address_of record, column);
                                if (column == TOOLS_UUID_COLUMN_TIME &&
                                    record.valid && !string_get(value))
                                        text_put_string("null");
                                else
                                        tools_uuid_json_string(value);
                                text_put_string(at + 1 < column_count
                                                    ? ",\n" : "\n");
                        }
                        text_put_string("      }");
                }
                text_put_string(file_operand_count
                                    ? "\n   ]\n}\n" : "\n   ]\n}\n");
                return text_done(0);
        }

        positive widths[32];
        for (positive at = 0; at < column_count; at++)
                widths[at] = noheadings ? 0
                                        : string_length(
                                              tools_uuid_column_names[columns[at]]);

        for (positive row = 0; row < file_operand_count; row++)
        {
                tools_uuid_record record;
                tools_uuid_record_read(file_operand_at(row), address_of record);
                for (positive at = 0; at < column_count; at++)
                        widths[at] = max(widths[at],
                                         string_length(tools_uuid_cell(
                                             address_of record, columns[at])));
        }

        if (!raw)
                for (positive at = 0; at < column_count; at++)
                        if (columns[at] == TOOLS_UUID_COLUMN_UUID)
                                widths[at] = max(widths[at], (positive)36);
                        else if (columns[at] == TOOLS_UUID_COLUMN_VARIANT)
                                widths[at] = max(widths[at], noheadings
                                                                ? (positive)9
                                                                : (positive)7);
                        else if (columns[at] == TOOLS_UUID_COLUMN_TYPE)
                                widths[at] = max(widths[at], (positive)10);

        if (!noheadings)
        {
                for (positive at = 0; at < column_count; at++)
                {
                        if (at)
                                writer_fill(text_put,
                                            !raw && columns[at - 1] ==
                                                            TOOLS_UUID_COLUMN_UUID
                                                ? 2 : 1,
                                            ' ');
                        string_address value = tools_uuid_column_names[columns[at]];
                        positive length = string_length(value);
                        if (raw)
                                tools_uuid_safe_cell(value);
                        else
                                text_put((p8 address_to)value, length);
                        if (!raw && at + 1 < column_count)
                                writer_fill(text_put, widths[at] - length, ' ');
                }
                text_put_character('\n');
        }

        for (positive row = 0; row < file_operand_count; row++)
        {
                tools_uuid_record record;
                tools_uuid_record_read(file_operand_at(row), address_of record);
                for (positive at = 0; at < column_count; at++)
                {
                        if (at)
                                writer_fill(text_put,
                                            !raw && columns[at - 1] ==
                                                            TOOLS_UUID_COLUMN_UUID
                                                ? 2 : 1,
                                            ' ');
                        string_address value =
                            tools_uuid_cell(address_of record, columns[at]);
                        positive length = string_length(value);
                        if (raw)
                                tools_uuid_safe_cell(value);
                        else
                                text_put((p8 address_to)value, length);
                        if (!raw && at + 1 < column_count)
                                writer_fill(text_put, widths[at] - length, ' ');
                }
                text_put_character('\n');
        }

        return text_done(0);
}

static const file_long tools_mcookie_longs[] = {
    {(string_address)"file", 'f'},
    {(string_address)"max-size", 'm'},
    {(string_address)"verbose", 'v'},
    {null, 0},
};

static fn tools_mcookie_mix(file_random_state address_to random,
                            p8 address_to bytes, positive length,
                            positive address_to position)
{
        for (positive at = 0; at < length; at++, (address_to position)++)
        {
                positive place = address_to position;
                random->words[place & 3] ^=
                    (p64)bytes[at] << (((place >> 2) & 7) * 8);
                if ((place & 31) == 31)
                        (void)file_random_word(random);
        }
}

static b32 tools_mcookie()
{
        file_taking taking = {
            .program = (string_address)"mcookie",
            .allowed = (string_address)"fmv",
            .valued = (string_address)"fm",
            .longs = tools_mcookie_longs,
        };

        text_begin("mcookie");
        if (!file_take(address_of taking))
                return text_done(1);

        positive maximum = 4096;
        string_address maximum_text = file_option_value(address_of taking, 'm');
        if (maximum_text &&
            !(string_is(maximum_text, '0') && !string_get(maximum_text + 1)) &&
            !split_size(maximum_text, address_of maximum))
                return text_refuse(maximum_text, "invalid maximum size", 1);

        file_random_state random;
        if (!file_random_seed(address_of random))
                return text_refuse(null, "kernel random source failed", 1);

        /* Upstream draws 128 kernel bytes.  Four uses of the existing
           32-byte seeder preserve that entropy budget without a direct
           getrandom implementation beside file_random_seed. */
        for (positive round = 1; round < 4; round++)
        {
                file_random_state additional;
                if (!file_random_seed(address_of additional))
                        return text_refuse(null, "kernel random source failed", 1);
                for (positive at = 0; at < array_count(random.words); at++)
                        random.words[at] ^= additional.words[at];
                (void)file_random_word(address_of random);
        }

        string_address path = file_option_value(address_of taking, 'f');
        positive mixed = 0;
        if (path && maximum)
        {
                bipolar handle = string_is(path, '-') && !string_get(path + 1)
                                     ? 0
                                     : system_open_at(AT_FDCWD, path,
                                                      FILE_READ | O_CLOEXEC);
                if (handle < 0)
                        text_error(path, file_reason(handle));
                else
                {
                        while (mixed < maximum)
                        {
                                positive want = min(maximum - mixed,
                                                    sizeof(file_transfer));
                                bipolar got = system_read_retry(
                                    (positive)handle, file_transfer, want);
                                if (got < 0)
                                {
                                        text_error(path, file_reason(got));
                                        break;
                                }
                                if (!got)
                                        break;
                                tools_mcookie_mix(address_of random,
                                                  file_transfer, (positive)got,
                                                  address_of mixed);
                        }
                        if (handle)
                                system_close((positive)handle);
                }
        }

        if (taking.flags & FILE_FLAG('v'))
        {
                if (path)
                {
                        text_error_raw("Got ");
                        p8 number[24];
                        positive length = positive_into_string(number, mixed);
                        number[length] = end;
                        text_error_raw(number);
                        text_error_raw(" bytes from ");
                        text_error_raw(path);
                        text_error_raw("\n");
                }
                text_error_raw("Got 128 bytes from getrandom() function\n");
        }

        for (positive warm = 0; warm < 8; warm++)
                (void)file_random_word(address_of random);
        tools_uuid cookie;
        tools_uuid_random_bytes(address_of random, address_of cookie);
        for (positive at = 0; at < sizeof(cookie.bytes); at++)
        {
                text_put_character(storage_hex_digit(cookie.bytes[at] >> 4,
                                                     false));
                text_put_character(storage_hex_digit(cookie.bytes[at] & 15,
                                                     false));
        }
        text_put_character('\n');
        return text_done(0);
}


// dd --------------------------------------------------------

#define DD_NOTRUNC 0x001
#define DD_SYNC 0x002
#define DD_NOERROR 0x004
#define DD_FSYNC 0x008
#define DD_FDATASYNC 0x010
#define DD_EXCL 0x020
#define DD_NOCREAT 0x040
#define DD_LCASE 0x080
#define DD_UCASE 0x100
#define DD_SWAB 0x200

#define DD_FULLBLOCK 0x001
#define DD_APPEND 0x001
#define DD_O_APPEND 02000

#define DD_STATUS_ALL 0
#define DD_STATUS_NOXFER 1
#define DD_STATUS_NONE 2

#define DD_SIGNAL_INFO 10
#define DD_NO_SUCH_CALL 38

static positive dd_in_full;
static positive dd_in_partial;
static positive dd_out_full;
static positive dd_out_partial;
static positive dd_written;
static positive dd_status_level;
static positive dd_started;

// Set in the handler, acted on where a block boundary is, because printing
// the summary from inside the handler would land it in the middle of one.
static volatile b32 dd_info_asked;

static fn dd_info_caught(b32 number)
{
        (void)number;
        dd_info_asked = 1;
}

static fn dd_say_number(positive value)
{
        p8 digits[24];
        positive length = positive_into(digits, value);

        system_write_all(2, digits, length);
}

// A scaled count with no prefix letter is the plain number again, and dd
// leaves the parenthesis off rather than saying the same thing twice.
static bool dd_bare(p8 address_to text, positive length)
{
        return length >= 2 && text[length - 2] == ' ';
}

static fn dd_summary()
{
        if (dd_status_level == DD_STATUS_NONE)
                return;

        text_flush();

        dd_say_number(dd_in_full);
        text_error_raw("+");
        dd_say_number(dd_in_partial);
        text_error_raw(" records in\n");
        dd_say_number(dd_out_full);
        text_error_raw("+");
        dd_say_number(dd_out_partial);
        text_error_raw(" records out\n");

        if (dd_status_level == DD_STATUS_NOXFER)
                return;

        p8 si[32];
        p8 iec[32];
        positive si_length = positive_into_human_nearest_string(si, dd_written,
                                                                 false);
        positive iec_length = positive_into_human_nearest_string(iec, dd_written,
                                                                  true);

        dd_say_number(dd_written);
        text_error_raw(dd_written == 1 ? " byte" : " bytes");

        if (!dd_bare(si, si_length))
        {
                text_error_raw(" (");
                text_error_raw(si);

                if (!dd_bare(iec, iec_length))
                {
                        text_error_raw(", ");
                        text_error_raw(iec);
                }

                text_error_raw(")");
        }

        /*
                The seconds and the rate are what this machine did, not what
                the other one did, so they are printed in the shape coreutils
                prints them in and nothing here compares them.
        */
        p64 wall[2] = {0, 0};

        system_call_2(syscall(clock_gettime), 1, (positive)wall);

        positive elapsed = (positive)wall[0] * 1000000000u + (positive)wall[1] - dd_started;

        if (!elapsed)
                elapsed = 1;

        text_error_raw(" copied, ");

        positive whole = elapsed / 1000000000u;
        positive rest = elapsed % 1000000000u;

        dd_say_number(whole);
        text_error_raw(".");

        p8 fraction[9];
        positive fraction_length = positive_into_padded(fraction, rest, 9, '0');

        system_write_all(2, fraction, fraction_length);

        text_error_raw(" s, ");

        /*
                Bytes per second from microseconds, not from whole seconds:
                1.9 s rounded down to 1 s said a rate nearly twice the true
                one. A count too large to scale up first is scaled after the
                division instead, at a precision nobody reading a terabyte's
                rate will miss.
        */
        p8 rate[32];
        positive microseconds = elapsed / 1000u;

        if (!microseconds)
                microseconds = 1;

        positive per = dd_written <= positive_max / 1000000u
                           ? dd_written * 1000000u / microseconds
                           : dd_written / microseconds * 1000000u;

        positive_into_human_nearest_string(rate, per, false);
        text_error_raw(rate);
        text_error_raw("/s\n");
}

static bool dd_size(string_address text, positive address_to out)
{
        positive total = 1;
        string_address at = text;

        if (!string_get(at))
                return false;

        while (1)
        {
                positive value;

                if (!string_digits_checked(address_of at, 10, address_of value))
                        return false;

                positive power = file_size_power(string_get(at), false);
                positive multiple = 1;

                if (power)
                        at++;
                else switch (string_get(at))
                {
                case 'b': multiple = 512; at++; break;
                case 'c': at++; break;
                case 'w': multiple = 2; at++; break;
                case 'B': at++; break;
                }

                if (power)
                {
                        positive base = 1024;

                        // KiB is the binary one spelled out; KB is the
                        // decimal one, and a bare K is binary.
                        if (string_get(at) == 'i' && string_get(at + 1) == 'B')
                        {
                                at += 2;
                        }
                        else if (string_get(at) == 'B' || string_get(at) == 'D')
                        {
                                base = 1000;
                                at++;
                        }

                        for (positive i = 0; i < power; i++)
                        {
                                if (multiple > positive_max / base)
                                        return false;

                                multiple *= base;
                        }
                }

                if (value && multiple > positive_max / value)
                        return false;

                positive piece = value * multiple;

                if (piece && total > positive_max / piece)
                        return false;

                total *= piece;

                if (string_get(at) != 'x')
                        break;

                at++;
        }

        if (string_get(at))
                return false;

        address_to out = total;

        return true;
}

// A final B on count, skip or seek changes the unit from blocks to bytes.
// It is still part of the ordinary size grammar (3KB is 3000), so parsing is
// shared and only this last-byte fact is carried separately.
static bool dd_quantity(string_address text, positive address_to out,
                        bool address_to bytes)
{
        positive length = string_length(text);

        address_to bytes = length && text[length - 1] == 'B';
        return dd_size(text, out);
}

// name=value, which is the grammar an environment entry has.
static bool dd_operand(string_address argument, string_address name,
                       string_address address_to value)
{
        positive length = string_length(name);

        if (!environment_key_is(argument, name, length))
                return false;

        address_to value = argument + length + 1;

        return true;
}

static bool dd_word(string_address address_to at, string_address name)
{
        string_address here = address_to at;
        positive i = string_length(name);

        if (string_compare_max(here, name, i))
                return false;

        if (here[i] && here[i] != ',')
                return false;

        address_to at = here + i + (here[i] == ',' ? 1 : 0);

        return true;
}

/*
        Every complaint dd makes about a file is the same sentence: dd, what
        went wrong, which file, and the kernel's reason when there is one.
        The verb sits before the name when it is an act that failed, "failed
        to open 'x'", and after it when it is a finding about the file, "'x':
        cannot seek", which is the only difference between the two shapes
        coreutils uses. One writer keeps the ten of them from drifting
        apart.
*/
static fn dd_complain(string_address before, string_address name,
                      string_address fallback, string_address after,
                      bipolar code)
{
        text_flush();
        text_error_raw("dd: ");

        if (before)
        {
                text_error_raw(before);
                text_error_raw(" ");
        }

        text_error_raw("'");
        text_error_raw(name ? name : fallback);
        text_error_raw("'");

        if (after)
        {
                text_error_raw(": ");
                text_error_raw(after);
        }

        if (code < 0)
        {
                text_error_raw(": ");
                text_error_raw(file_reason(code));
        }

        text_error_raw("\n");
}

/*
        Every output path has the same failure contract. Keeping it here
        prevents regrouped blocks, the final partial block and seek padding
        from quietly accepting a short write while the equal-size fast path
        reports it.
*/
static positive dd_output(positive handle, string_address name,
                          p8 address_to bytes, positive length, bool copied)
{
        positive wrote = system_write_all(handle, bytes, length);

        if (copied)
                dd_written += wrote;

        if (wrote != length)
                dd_complain("error writing", name, "standard output", null, 0);

        return wrote;
}

/*
        ftruncate refused after a seek, and whether that is dd's failure.
        POSIX says what truncation does to a regular file, a directory and a
        shared memory object and nothing about the rest, so coreutils reports
        a refusal from those and goes on copying past one from a device or a
        pipe, where "invalid argument" was never going to mean anything.
*/
static bool dd_truncate_failed(positive handle, string_address output,
                               positive length, bipolar refused)
{
        file_facts facts;

        // The refusal's errno is part of dd's message, and file_look folds
        // it into a bool, so this is the bare statx that file_look wraps.
        bipolar told = system_stat_at(handle, "",
                                      AT_EMPTY_PATH | AT_NO_AUTOMOUNT,
                                      STATX_BASIC, address_of facts);

        if (told < 0)
        {
                dd_complain("cannot fstat", output, "standard output", null,
                            told);
                return true;
        }

        positive kind = facts.mode & MODE_FORMAT;

        if (kind != MODE_FILE && kind != MODE_DIRECTORY)
                return false;

        p8 sentence[72];
        positive at = 22;

        memory_copy_apart(sentence, "failed to truncate to ", at);
        at += positive_into(sentence + at, length);
        string_copy(sentence + at, " bytes in output file");
        dd_complain(sentence, output, "standard output", null, refused);
        return true;
}

// swab is one conversion over the byte stream, not one conversion per read.
// An odd byte therefore waits for the first byte of the next input record.
static positive dd_swab(p8 address_to into, p8 address_to from, positive length,
                        bool address_to pending, p8 address_to held)
{
        positive in = 0;
        positive out = 0;

        if (address_to pending && length)
        {
                into[out++] = from[in++];
                into[out++] = address_to held;
                address_to pending = false;
        }

        while (in + 1 < length)
        {
                into[out++] = from[in + 1];
                into[out++] = from[in];
                in += 2;
        }

        if (in < length)
        {
                address_to held = from[in];
                address_to pending = true;
        }

        return out;
}

// A short read is not the end of the input, and a partial record is not an
// error: both are counted and the next block is asked for.
static b32 tools_dd(void)
{
        string_address input = null;
        string_address output = null;
        positive ibs = 512;
        positive obs = 512;
        positive bs = 0;
        positive count = TEXT_UNSET;
        positive skip = 0;
        positive seek = 0;
        positive cbs = 0;
        positive conv = 0;
        positive iflags = 0;
        positive oflags = 0;
        bool bs_set = false;
        bool count_set = false;
        bool count_bytes = false;
        bool skip_bytes = false;
        bool seek_bytes = false;
        bool count_bytes_flag = false;
        bool skip_bytes_flag = false;
        bool seek_bytes_flag = false;
        b32 status = 0;

        text_begin("dd");

        dd_in_full = dd_in_partial = dd_out_full = dd_out_partial = 0;
        dd_written = 0;
        dd_status_level = DD_STATUS_ALL;
        dd_info_asked = 0;
        text_arena_used = 0;

        {
                p64 wall[2] = {0, 0};

                system_call_2(syscall(clock_gettime), 1, (positive)wall);
                dd_started = (positive)wall[0] * 1000000000u + (positive)wall[1];
        }

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = program_argument(i);
                string_address value;

                if (dd_operand(argument, "if", address_of value))
                        input = value;
                else if (dd_operand(argument, "of", address_of value))
                        output = value;
                else if (dd_operand(argument, "ibs", address_of value))
                {
                        if (!dd_size(value, address_of ibs))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "obs", address_of value))
                {
                        if (!dd_size(value, address_of obs))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "bs", address_of value))
                {
                        if (!dd_size(value, address_of bs))
                                return text_error(argument, "invalid number"), 1;

                        bs_set = true;
                }
                else if (dd_operand(argument, "count", address_of value))
                {
                        if (!dd_quantity(value, address_of count,
                                         address_of count_bytes))
                                return text_error(argument, "invalid number"), 1;

                        count_set = true;
                }
                else if (dd_operand(argument, "skip", address_of value) ||
                         dd_operand(argument, "iseek", address_of value))
                {
                        if (!dd_quantity(value, address_of skip,
                                         address_of skip_bytes))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "seek", address_of value) ||
                         dd_operand(argument, "oseek", address_of value))
                {
                        if (!dd_quantity(value, address_of seek,
                                         address_of seek_bytes))
                                return text_error(argument, "invalid number"), 1;
                }
                else if (dd_operand(argument, "status", address_of value))
                {
                        if (string_equals(value, "none"))
                                dd_status_level = DD_STATUS_NONE;
                        else if (string_equals(value, "noxfer"))
                                dd_status_level = DD_STATUS_NOXFER;
                        else if (string_equals(value, "progress"))
                                dd_status_level = DD_STATUS_ALL;
                        else
                                return text_error(value, "invalid status level"), 1;
                }
                else if (dd_operand(argument, "conv", address_of value))
                {
                        string_address at = value;

                        while (string_get(at))
                        {
                                if (dd_word(address_of at, "notrunc"))
                                        conv |= DD_NOTRUNC;
                                else if (dd_word(address_of at, "sync"))
                                        conv |= DD_SYNC;
                                else if (dd_word(address_of at, "noerror"))
                                        conv |= DD_NOERROR;
                                else if (dd_word(address_of at, "fdatasync"))
                                        conv |= DD_FDATASYNC;
                                else if (dd_word(address_of at, "fsync"))
                                        conv |= DD_FSYNC;
                                else if (dd_word(address_of at, "excl"))
                                        conv |= DD_EXCL;
                                else if (dd_word(address_of at, "nocreat"))
                                        conv |= DD_NOCREAT;
                                else if (dd_word(address_of at, "lcase"))
                                        conv |= DD_LCASE;
                                else if (dd_word(address_of at, "ucase"))
                                        conv |= DD_UCASE;
                                else if (dd_word(address_of at, "swab"))
                                        conv |= DD_SWAB;
                                else
                                        return text_error(at, "invalid conversion"), 1;
                        }
                }
                else if (dd_operand(argument, "iflag", address_of value))
                {
                        string_address at = value;

                        if (!string_get(at))
                                return text_error(value, "invalid input flag"), 1;

                        while (string_get(at))
                                if (dd_word(address_of at, "fullblock"))
                                        iflags |= DD_FULLBLOCK;
                                else if (dd_word(address_of at, "count_bytes"))
                                        count_bytes_flag = true;
                                else if (dd_word(address_of at, "skip_bytes"))
                                        skip_bytes_flag = true;
                                else
                                        return text_error(at, "invalid input flag"), 1;
                }
                else if (dd_operand(argument, "oflag", address_of value))
                {
                        string_address at = value;

                        if (!string_get(at))
                                return text_error(value, "invalid output flag"), 1;

                        while (string_get(at))
                                if (dd_word(address_of at, "append"))
                                        oflags |= DD_APPEND;
                                else if (dd_word(address_of at, "seek_bytes"))
                                        seek_bytes_flag = true;
                                else
                                        return text_error(at, "invalid output flag"), 1;
                }
                else if (dd_operand(argument, "cbs", address_of value))
                {
                        // cbs has no effect until block or unblock is chosen,
                        // but it remains a number and nonsense must not pass.
                        if (!dd_size(value, address_of cbs))
                                return text_error(argument, "invalid number"), 1;
                }
                else
                {
                        text_error(argument, "unrecognized operand");
                        return 1;
                }
        }

        (void)cbs;

        if (bs_set)
                ibs = obs = bs;

        count_bytes |= count_bytes_flag;
        skip_bytes |= skip_bytes_flag;
        seek_bytes |= seek_bytes_flag;

        if ((conv & DD_LCASE) && (conv & DD_UCASE))
                return text_error(null, "cannot combine lcase and ucase"), 1;

        if (!ibs || !obs || ibs > positive_max - 31 || obs > positive_max - 31)
        {
                text_error(null, "invalid number");
                return 1;
        }

        if ((count_set && count > (positive)bipolar_max) ||
            (skip && skip > (positive)bipolar_max / (skip_bytes ? 1 : ibs)) ||
            (seek && seek > (positive)bipolar_max / (seek_bytes ? 1 : obs)))
        {
                text_error(null, "offset too large");
                return 1;
        }

        // The whole output blocks a seek covers, which is what coreutils
        // decides the truncation by; a byte seek short of one block is
        // none of them.
        positive seek_records = seek_bytes ? seek / obs : seek;
        positive in_handle = 0;
        positive out_handle = 1;

        if (input)
        {
                bipolar opened = text_open_handle(input, FILE_READ, 0);

                if (opened < 0)
                {
                        dd_complain("failed to open", input,
                                    "standard input", null, opened);
                        return 1;
                }

                in_handle = (positive)opened;
        }

        if (output)
        {
                positive flags = 01;

                if (!(conv & DD_NOCREAT))
                        flags |= 0100;

                if (conv & DD_EXCL)
                        flags |= 0200;

                if (oflags & DD_APPEND)
                        flags |= DD_O_APPEND;

                // coreutils cuts the file at the seek rather than at its
                // start when the seek is whole blocks, and only then: a byte
                // seek short of one block truncates on open.
                if (!(conv & DD_NOTRUNC) && !seek_records)
                        flags |= O_TRUNC;

                bipolar opened = text_open_handle(output, flags, 0666);

                if (opened < 0)
                {
                        dd_complain("failed to open", output,
                                    "standard output", null, opened);
                        return 1;
                }

                out_handle = (positive)opened;
        }

        p8 address_to ibuf = (p8 address_to)text_arena_take(ibs + 16);
        p8 address_to obuf = ibs == obs && !(conv & DD_SWAB)
                                 ? ibuf
                                 : (p8 address_to)text_arena_take(obs + 16);
        p8 address_to converted = conv & DD_SWAB
                                      ? (p8 address_to)text_arena_take(ibs + 16)
                                      : ibuf;

        if (!ibuf || !obuf || !converted)
                return 1;

        /* Restart a read interrupted by the report signal, so a short read is
           not mistaken for a partial input record. */
        system_signal_install(DD_SIGNAL_INFO, (positive)dd_info_caught,
                              SIGNAL_CATCH_FLAGS, SIGNAL_CATCH_RESTORER, null);

        if (skip)
        {
                positive want = skip_bytes ? skip : skip * ibs;
                // From where the input already is, not from its start: a dd
                // reading after another command on the same input skips
                // from where that command stopped.
                bipolar landed = system_seek(in_handle, want, 1);
                bool short_of_it = false;

                if (landed >= 0)
                {
                        bipolar stop = system_seek(in_handle, 0, 2);

                        if (stop >= 0)
                        {
                                system_seek(in_handle, landed, 0);

                                // A size of zero is what a file that has no
                                // size to report says, so it is not a file
                                // that is too short.
                                if (stop > 0 && stop < landed)
                                        short_of_it = true;
                        }
                }
                else
                {
                        positive left = want;

                        while (left)
                        {
                                positive ask = left < ibs ? left : ibs;
                                bipolar got = system_read_retry(in_handle, ibuf, ask);

                                if (got <= 0)
                                        break;

                                left -= (positive)got;
                        }

                        short_of_it = left != 0;
                }

                // Asked to skip past what is there. Not fatal, and said only
                // where a summary would have been said.
                if (short_of_it && dd_status_level != DD_STATUS_NONE)
                        dd_complain(null, input, "standard input",
                                    "cannot skip to specified offset", 0);
        }

        /*
                seek moves the output on from where it already is, so a dd
                sharing its standard output with the command before it
                writes after what that command wrote. What is cut off after
                the seek is coreutils' rule exactly: only a file dd opened
                itself, only to a seek of whole blocks, and a refusal counts
                only from the kinds of file dd_truncate_failed names.
                Standard output is never truncated, whatever it is.
        */
        if (seek)
        {
                positive want = seek_bytes ? seek : seek * obs;
                bipolar landed = system_seek(out_handle, want, 1);

                if (landed < 0)
                {
                        dd_complain(null, output, "standard output",
                                    "cannot seek", landed);
                        status = 1;
                }
                else if (output && seek_records && !(conv & DD_NOTRUNC))
                {
                        bipolar refused =
                            system_truncate_handle(out_handle, want);

                        if (refused < 0 &&
                            dd_truncate_failed(out_handle, output, want,
                                               refused))
                                status = 1;
                }
        }

        positive held = 0;
        b32 result = status;
        positive partial_before = 0;
        positive input_bytes = 0;
        bool swab_pending = false;
        p8 swab_held = 0;

        while (count != 0 && !result)
        {
                if (dd_info_asked)
                {
                        dd_info_asked = 0;
                        dd_summary();
                }

                if (!count_bytes && count != TEXT_UNSET &&
                    dd_in_full + dd_in_partial >= count)
                        break;

                positive ask = ibs;

                if (count_bytes && count != TEXT_UNSET)
                {
                        if (input_bytes >= count)
                                break;

                        if (ask > count - input_bytes)
                                ask = count - input_bytes;
                }

                if (conv & (DD_SYNC | DD_NOERROR))
                        memory_fill(ibuf, 0, ibs);

                bipolar got;

                if (iflags & DD_FULLBLOCK)
                {
                        positive gathered = 0;

                        while (gathered < ask)
                        {
                                got = system_read_retry(in_handle, ibuf + gathered,
                                                        ask - gathered);

                                if (got <= 0)
                                        break;

                                gathered += (positive)got;
                        }

                        if (gathered)
                                got = (bipolar)gathered;
                }
                else
                {
                        got = system_read_retry(in_handle, ibuf, ask);
                }

                if (!got)
                        break;

                if (got < 0)
                {
                        dd_complain("error reading", input, "standard input",
                                    null, got);

                        if (!(conv & DD_NOERROR))
                        {
                                result = 1;
                                break;
                        }

                        dd_summary();

                        positive bad = ibs - partial_before;

                        if (system_seek(in_handle, bad, 1) < 0)
                                result = 1;

                        if ((conv & DD_SYNC) && !partial_before)
                                got = 0;
                        else
                                continue;
                }

                positive read_bytes = (positive)got;

                input_bytes += read_bytes;

                if (read_bytes < ibs)
                {
                        dd_in_partial++;
                        partial_before = read_bytes;

                        if (conv & DD_SYNC)
                                read_bytes = ibs;
                }
                else
                {
                        dd_in_full++;
                        partial_before = 0;
                }

                if (conv & DD_LCASE)
                        memory_to_lower_ascii(ibuf, read_bytes);

                if (conv & DD_UCASE)
                        memory_to_upper_ascii(ibuf, read_bytes);

                p8 address_to output_bytes = ibuf;

                if (conv & DD_SWAB)
                {
                        read_bytes = dd_swab(converted, ibuf, read_bytes,
                                             address_of swab_pending,
                                             address_of swab_held);
                        output_bytes = converted;
                }

                if (ibuf == obuf)
                {
                        positive wrote = dd_output(out_handle, output, obuf,
                                                   read_bytes, true);

                        if (wrote != read_bytes)
                        {
                                if (wrote)
                                        dd_out_partial++;

                                result = 1;
                                break;
                        }

                        if (read_bytes == ibs)
                                dd_out_full++;
                        else
                                dd_out_partial++;

                        continue;
                }

                // The input block regrouped into output blocks, which is what
                // dd is for whenever ibs and obs differ.
                for (positive at = 0; at < read_bytes;)
                {
                        positive take = obs - held;

                        if (take > read_bytes - at)
                                take = read_bytes - at;

                        memory_copy_apart(obuf + held, output_bytes + at, take);
                        held += take;
                        at += take;

                        if (held < obs)
                                continue;

                        positive wrote = dd_output(out_handle, output, obuf, obs,
                                                   true);
                        held = 0;

                        if (wrote != obs)
                        {
                                if (wrote)
                                        dd_out_partial++;

                                result = 1;
                                break;
                        }

                        dd_out_full++;
                }

                if (result)
                        break;
        }

        if (swab_pending)
                obuf[held++] = swab_held;

        if (held)
        {
                positive wrote = dd_output(out_handle, output, obuf, held, true);

                if (wrote)
                        dd_out_partial++;

                if (wrote != held)
                        result = 1;
        }

        /*
                A stream that cannot be synced says so and is a failure. A
                pipe answers the narrower call with "invalid argument", and
                dd asks the wider one rather than giving up, which is why the
                complaint about a pipe names fsync even when nobody asked for
                it.
        */
        if (conv & DD_FDATASYNC)
        {
                bipolar done = system_call_1(syscall(fdatasync), out_handle);

                if (done == -ERROR_INVALID || done == -DD_NO_SUCH_CALL)
                {
                        conv |= DD_FSYNC;
                }
                else if (done < 0)
                {
                        dd_complain("fdatasync failed for", output,
                                    "standard output", null, done);
                        result = 1;
                }
        }

        if (conv & DD_FSYNC)
        {
                bipolar done = system_call_1(syscall(fsync), out_handle);

                if (done < 0)
                {
                        dd_complain("fsync failed for", output,
                                    "standard output", null, done);
                        result = 1;
                }
        }

        if (out_handle != 1 && system_close(out_handle) < 0)
        {
                text_error(output, "close failed");
                result = 1;
        }

        if (in_handle != 0)
                system_close(in_handle);

        text_flush();
        dd_summary();

        return result;
}

// Binary dumps -------------------------------------------------------------

/*
        od and hexdump are two front ends to one sixteen-byte streaming dump.

        The input is the text utilities' existing 64 KiB reader and output is
        their existing buffered writer.  The only local storage is one input
        row, the previous row used for `*` suppression, and one completed
        output line.  In particular, neither personality has a byte-at-a-time
        syscall loop or its own allocation and option machinery.

        A format records only the differences between the public spellings.
        Canonical hexdump is a special row; all the integer and character
        rows share the same loader and field emitters.
*/
#define DUMP_BLOCK 16
#define DUMP_FORMAT_MAX 16
#define DUMP_INTEGER 0
#define DUMP_CHARACTER 1
#define DUMP_CANONICAL 2

typedef struct
{
        p8 kind;
        p8 base;
        p8 size;
        p8 width;
        p8 gap;
        bool signed_value;
        bool zero;
        bool printable;
        bool hexdump;
} dump_format;

typedef struct
{
        dump_format format[DUMP_FORMAT_MAX];
        positive count;
        positive skip;
        positive limit;
        p8 address_base;
        p8 address_width;
        bool address_none;
        bool duplicates;
        bool od;
        bool failed;
} dump_options;

static dump_options dump_arguments;

static fn dump_add_integer(positive base, positive size, positive width,
                           positive gap, bool signed_value, bool zero,
                           bool printable, bool hexdump)
{
        if (dump_arguments.count >= DUMP_FORMAT_MAX)
        {
                dump_arguments.failed = true;
                return;
        }

        dump_arguments.format[dump_arguments.count++] = (dump_format){
            .kind = DUMP_INTEGER,
            .base = (p8)base,
            .size = (p8)size,
            .width = (p8)width,
            .gap = (p8)gap,
            .signed_value = signed_value,
            .zero = zero,
            .printable = printable,
            .hexdump = hexdump,
        };
}

static fn dump_add_character(bool hexdump)
{
        if (dump_arguments.count >= DUMP_FORMAT_MAX)
        {
                dump_arguments.failed = true;
                return;
        }

        dump_arguments.format[dump_arguments.count++] = (dump_format){
            .kind = DUMP_CHARACTER,
            .size = 1,
            .width = 3,
            .gap = 1,
            .hexdump = hexdump,
        };
}

static fn dump_add_canonical()
{
        if (dump_arguments.count >= DUMP_FORMAT_MAX)
        {
                dump_arguments.failed = true;
                return;
        }

        dump_arguments.format[dump_arguments.count++] = (dump_format){
            .kind = DUMP_CANONICAL,
            .size = 1,
            .hexdump = true,
        };
}

static bool dump_number(string_address source, positive address_to value)
{
        return source && dd_size(source, value);
}

/* GNU's integer type widths are the width of the widest value, including a
   possible minus.  Keeping that table here also makes every row take the
   tuned positive_into_base path rather than a miniature formatter. */
static positive dump_od_width(p8 type, positive size)
{
        if (type == 'x')
                return size * 2;

        if (type == 'o')
                return size == 1 ? 3 : size == 2 ? 6 : size == 4 ? 11 : 22;

        if (type == 'u')
                return size == 1 ? 3 : size == 2 ? 5 : size == 4 ? 10 : 20;

        return size == 1 ? 4 : size == 2 ? 6 : size == 4 ? 11 : 20;
}

/* One -t word can hold several formats (`-t x1c`) and z decorates the
   integer format immediately before it.  Floating point and the named C
   sizes are intentionally refused instead of being interpreted nearly. */
static bool dump_od_types(string_address word)
{
        positive at = 0;

        if (!word || !word[0])
                return false;

        while (word[at])
        {
                p8 type = word[at++];

                if (type == 'c')
                {
                        dump_add_character(false);
                        continue;
                }

                if (type != 'd' && type != 'o' && type != 'u' && type != 'x')
                        return false;

                positive size = 4;

                if (word[at] == '1' || word[at] == '2' ||
                    word[at] == '4' || word[at] == '8')
                        size = (positive)(word[at++] - '0');

                bool printable = word[at] == 'z';

                if (printable)
                        at++;

                dump_add_integer(type == 'd' || type == 'u' ? 10
                                 : type == 'o'                 ? 8
                                                               : 16,
                                 size, dump_od_width(type, size), 1,
                                 type == 'd', type == 'o' || type == 'x',
                                 printable, false);

                if (dump_arguments.failed)
                        return false;
        }

        return true;
}

static const file_long dump_od_longs[] = {
    {(string_address) "address-radix", 'A'},
    {(string_address) "skip-bytes", 'j'},
    {(string_address) "read-bytes", 'N'},
    {(string_address) "format", 't'},
    {(string_address) "output-duplicates", 'v'},
    {null, 0},
};

static bool dump_od_seen(p8 letter, string_address value)
{
        if (letter == 't' && !dump_od_types(value))
        {
                text_error(value, "unsupported output format");
                return false;
        }

        return true;
}

static const file_long dump_hex_longs[] = {
    {(string_address) "one-byte-octal", 'b'},
    {(string_address) "one-byte-char", 'c'},
    {(string_address) "canonical", 'C'},
    {(string_address) "two-bytes-decimal", 'd'},
    {(string_address) "two-bytes-octal", 'o'},
    {(string_address) "two-bytes-hex", 'x'},
    {(string_address) "length", 'n'},
    {(string_address) "skip", 's'},
    {(string_address) "no-squeezing", 'v'},
    {null, 0},
};

/* hexdump permits more than one stock display and writes them in command-line
   order.  file_take's seen hook preserves that order without a second option
   parser. */
static bool dump_hex_seen(p8 letter, string_address value)
{
        (void)value;

        switch (letter)
        {
        case 'b': dump_add_integer(8, 1, 3, 1, false, true, false, true); break;
        case 'c': dump_add_character(true); break;
        case 'C': dump_add_canonical(); break;
        case 'd': dump_add_integer(10, 2, 5, 3, false, true, false, true); break;
        case 'o': dump_add_integer(8, 2, 6, 2, false, true, false, true); break;
        case 'x': dump_add_integer(16, 2, 4, 4, false, true, false, true); break;
        }

        if (dump_arguments.failed)
        {
                text_error(null, "too many output formats");
                return false;
        }

        return true;
}

static positive dump_pad(p8 address_to into, positive count, p8 byte)
{
        memory_fill(into, byte, count);
        return count;
}

static positive dump_unsigned_field(p8 address_to into, positive value,
                                    positive base, positive width, p8 pad)
{
        p8 digits[24];
        positive length = positive_into_base(digits, value, base, false);
        positive made = 0;

        if (length < width)
                made += dump_pad(into, width - length, pad);

        memory_copy_apart(into + made, digits, length);
        return made + length;
}

static positive dump_signed_field(p8 address_to into, positive value,
                                  positive size, positive width)
{
        positive bits = size * 8;
        positive sign = (positive)1 << (bits - 1);
        positive mask = bits == positive_bits ? positive_max
                                               : ((positive)1 << bits) - 1;
        bool negative = (value & sign) != 0;
        positive magnitude = negative ? ((~value + 1) & mask) : value;
        p8 digits[24];
        positive length = positive_into_base(digits, magnitude, 10, false);
        positive body = length + (negative ? 1 : 0);
        positive made = 0;

        if (body < width)
                made += dump_pad(into, width - body, ' ');

        if (negative)
                into[made++] = '-';

        memory_copy_apart(into + made, digits, length);
        return made + length;
}

static positive dump_value(p8 address_to bytes, positive have, positive size)
{
        positive value = 0;

        if (have > size)
                have = size;

        /* All three supported ABIs are little-endian.  Loading explicitly
           also avoids an unaligned word load at every field. */
        for (positive at = 0; at < have; at++)
                value |= (positive)bytes[at] << (at * 8);

        return value;
}

static positive dump_character_field(p8 address_to into, p8 value)
{
        p8 spelling[3];
        positive length = 1;

        spelling[0] = value;

        switch (value)
        {
        case 0: spelling[0] = '\\'; spelling[1] = '0'; length = 2; break;
        case 7: spelling[0] = '\\'; spelling[1] = 'a'; length = 2; break;
        case 8: spelling[0] = '\\'; spelling[1] = 'b'; length = 2; break;
        case 9: spelling[0] = '\\'; spelling[1] = 't'; length = 2; break;
        case 10: spelling[0] = '\\'; spelling[1] = 'n'; length = 2; break;
        case 11: spelling[0] = '\\'; spelling[1] = 'v'; length = 2; break;
        case 12: spelling[0] = '\\'; spelling[1] = 'f'; length = 2; break;
        case 13: spelling[0] = '\\'; spelling[1] = 'r'; length = 2; break;
        default:
                if (!byte_is_printable(value))
                        return dump_unsigned_field(into, value, 8, 3, '0');
        }

        positive made = dump_pad(into, 3 - length, ' ');
        memory_copy_apart(into + made, spelling, length);
        return made + length;
}

static positive dump_address(p8 address_to into, positive address,
                             positive base, positive width)
{
        return dump_unsigned_field(into, address, base, width, '0');
}

static fn dump_canonical_line(p8 address_to bytes, positive length,
                              positive address)
{
        p8 line[96];
        positive made = dump_address(line, address, 16, 8);

        line[made++] = ' ';
        line[made++] = ' ';

        for (positive at = 0; at < DUMP_BLOCK; at++)
        {
                if (at == 8)
                        line[made++] = ' ';

                if (at < length)
                {
                        made += dump_unsigned_field(line + made, bytes[at], 16,
                                                    2, '0');
                        line[made++] = ' ';
                }
                else
                        made += dump_pad(line + made, 3, ' ');
        }

        line[made++] = ' ';
        line[made++] = '|';

        for (positive at = 0; at < length; at++)
                line[made++] = byte_is_printable(bytes[at]) ? bytes[at] : '.';

        line[made++] = '|';
        line[made++] = '\n';
        text_put(line, made);
}

static fn dump_regular_line(dump_format address_to format,
                            p8 address_to bytes, positive length,
                            positive address, bool first)
{
        p8 line[192];
        positive made = 0;
        positive fields = (length + format->size - 1) / format->size;
        positive full_fields = DUMP_BLOCK / format->size;
        positive gap = format->gap;

        /* With several od formats GNU aligns their value columns to the
           widest selected row.  Derive the slot width from the formats
           already parsed rather than storing a second set of padded format
           descriptors. */
        if (dump_arguments.od && dump_arguments.count > 1)
        {
                positive widest = 0;

                for (positive at = 0; at < dump_arguments.count; at++)
                {
                        dump_format address_to other =
                            dump_arguments.format + at;

                        if (other->kind == DUMP_CANONICAL)
                                continue;

                        positive span = (other->gap + other->width) *
                                        (DUMP_BLOCK / other->size);

                        if (span > widest)
                                widest = span;
                }

                positive slot = widest / full_fields;

                if (slot > format->width)
                        gap = slot - format->width;
        }

        if (first || format->hexdump)
        {
                if (!dump_arguments.address_none)
                        made += dump_address(line, address,
                                             dump_arguments.address_base,
                                             dump_arguments.address_width);
        }
        else if (!dump_arguments.address_none)
                made += dump_pad(line, dump_arguments.address_width, ' ');

        for (positive field = 0; field < fields; field++)
        {
                made += dump_pad(line + made, gap, ' ');

                if (format->kind == DUMP_CHARACTER)
                        made += dump_character_field(line + made, bytes[field]);
                else
                {
                        positive left = length - field * format->size;
                        positive value = dump_value(bytes + field * format->size,
                                                    left, format->size);

                        if (format->signed_value)
                                made += dump_signed_field(line + made, value,
                                                          format->size,
                                                          format->width);
                        else
                                made += dump_unsigned_field(
                                    line + made, value, format->base,
                                    format->width, format->zero ? '0' : ' ');
                }
        }

        /* util-linux's stock formats are fixed-width records, including
           blanks for values absent from the final short row.  od does that
           only when its z suffix needs a stable printable column. */
        if (format->hexdump || format->printable)
                made += dump_pad(line + made,
                                 (full_fields - fields) *
                                     (gap + format->width),
                                 ' ');

        if (format->printable)
        {
                line[made++] = ' ';
                line[made++] = ' ';
                line[made++] = '>';

                for (positive at = 0; at < length; at++)
                        line[made++] = byte_is_printable(bytes[at])
                                           ? bytes[at]
                                           : '.';

                line[made++] = '<';
        }

        line[made++] = '\n';
        text_put(line, made);
}

static fn dump_row(p8 address_to bytes, positive length, positive address)
{
        for (positive at = 0; at < dump_arguments.count; at++)
        {
                dump_format address_to format = dump_arguments.format + at;

                if (format->kind == DUMP_CANONICAL)
                        dump_canonical_line(bytes, length, address);
                else
                        dump_regular_line(format, bytes, length, address,
                                          at == 0);
        }
}

/* Skip with one seek for an ordinary file.  procfs' size-zero regular files
   fail text_regular_size's probe and fall back to the same buffered read as a
   pipe, so the optimization never turns a dynamic pseudo-file into EOF. */
static positive dump_skip_input(positive wanted)
{
        positive size;

        if (wanted && text_regular_size(text_input.handle, address_of size))
        {
                bipolar here = system_seek(text_input.handle, 0, FILE_SEEK_CUR);

                if (here >= 0 && (positive)here <= size)
                {
                        positive available = size - (positive)here;
                        positive take = wanted < available ? wanted : available;

                        if (system_seek(text_input.handle, (positive)here + take,
                                        FILE_SEEK_SET) >= 0)
                                return take;
                }
        }

        positive taken = 0;

        while (taken < wanted && text_fill())
        {
                positive available = text_input.filled - text_input.position;
                positive take = wanted - taken < available ? wanted - taken
                                                            : available;

                text_input.position += take;
                taken += take;
        }

        return taken;
}

static b32 dump_run(positive first)
{
        p8 block[DUMP_BLOCK];
        p8 previous[DUMP_BLOCK];
        positive held = 0;
        positive offset = 0;
        positive skip = dump_arguments.skip;
        positive left = dump_arguments.limit;
        bool have_previous = false;
        bool starred = false;
        bool wrote = false;
        bool opened = false;
        positive count = (positive)text_argument_count;
        positive inputs = first < count ? count - first : 1;

        for (positive which = 0; which < inputs; which++)
        {
                if (!left && !skip && !(dump_arguments.od && which == 0))
                        break;

                string_address name = first < count
                                          ? program_argument((b32)(first + which))
                                          : null;

                if (!text_open(name))
                        continue;

                opened = true;

                if (skip)
                {
                        positive taken = dump_skip_input(skip);

                        skip -= taken;
                        offset += taken;
                }

                if (!left)
                {
                        text_close();
                        break;
                }

                while (!skip && left && text_fill())
                {
                        positive available = text_input.filled - text_input.position;

                        if (available > left)
                                available = left;

                        while (available)
                        {
                                positive take = DUMP_BLOCK - held;

                                if (take > available)
                                        take = available;

                                memory_copy_apart(block + held,
                                                  text_input.buffer +
                                                      text_input.position,
                                                  take);
                                held += take;
                                text_input.position += take;
                                available -= take;
                                left -= take;

                                if (held == DUMP_BLOCK)
                                {
                                        positive row_address = offset;
                                        offset += DUMP_BLOCK;

                                        if (!dump_arguments.duplicates &&
                                            have_previous &&
                                            !memory_compare(previous, block,
                                                            DUMP_BLOCK))
                                        {
                                                if (!starred)
                                                {
                                                        text_put("*\n", 2);
                                                        starred = true;
                                                }
                                        }
                                        else
                                        {
                                                dump_row(block, DUMP_BLOCK,
                                                         row_address);
                                                memory_copy(previous, block,
                                                            DUMP_BLOCK);
                                                have_previous = true;
                                                starred = false;
                                                wrote = true;
                                        }

                                        held = 0;
                                }
                        }
                }

                text_close();
        }

        if (skip && dump_arguments.od)
        {
                text_error(null, "cannot skip past end of combined input");
                return text_done(1);
        }

        if (held)
        {
                dump_row(block, held, offset);
                offset += held;
                wrote = true;
        }

        if ((dump_arguments.od && opened) || wrote ||
            (!dump_arguments.od && opened && dump_arguments.skip))
        {
                if (!dump_arguments.address_none)
                {
                        p8 final[32];
                        positive length = dump_address(final, offset,
                                                       dump_arguments.address_base,
                                                       dump_arguments.address_width);

                        final[length++] = '\n';
                        text_put(final, length);
                }
        }

        return text_done(text_status);
}

static b32 tools_od(void)
{
        file_taking taking = {
            .program = (string_address) "od",
            .allowed = (string_address) "AjNtv",
            .valued = (string_address) "AjNt",
            .longs = dump_od_longs,
            .seen = dump_od_seen,
        };

        text_begin("od");
        memory_fill(address_of dump_arguments, 0, sizeof(dump_arguments));
        dump_arguments.limit = TEXT_UNSET;
        dump_arguments.address_base = 8;
        dump_arguments.address_width = 7;
        dump_arguments.od = true;

        if (!file_take(address_of taking))
                return text_done(1);

        string_address radix = file_option_value(address_of taking, 'A');

        if (radix)
        {
                if (radix[0] == 'n' && !radix[1])
                        dump_arguments.address_none = true;
                else if (!radix[1] &&
                         (radix[0] == 'd' || radix[0] == 'o' ||
                          radix[0] == 'x'))
                {
                        dump_arguments.address_base = radix[0] == 'd' ? 10
                                                      : radix[0] == 'o' ? 8
                                                                        : 16;
                        dump_arguments.address_width = radix[0] == 'x' ? 6 : 7;
                }
                else
                        return text_refuse(radix, "invalid radix", 1);
        }

        if ((taking.flags & FILE_FLAG('j')) &&
            !dump_number(file_option_value(address_of taking, 'j'),
                         address_of dump_arguments.skip))
                return text_refuse(file_option_value(address_of taking, 'j'),
                                   "invalid skip", 1);

        if ((taking.flags & FILE_FLAG('N')) &&
            !dump_number(file_option_value(address_of taking, 'N'),
                         address_of dump_arguments.limit))
                return text_refuse(file_option_value(address_of taking, 'N'),
                                   "invalid byte count", 1);

        dump_arguments.duplicates = (taking.flags & FILE_FLAG('v')) != 0;

        if (!dump_arguments.count)
                dump_add_integer(8, 2, 6, 1, false, true, false, false);

        return dump_run(taking.first);
}

static b32 tools_hexdump(void)
{
        file_taking taking = {
            .program = (string_address) "hexdump",
            .allowed = (string_address) "bcCdoxnsv",
            .valued = (string_address) "ns",
            .longs = dump_hex_longs,
            .seen = dump_hex_seen,
        };

        text_begin("hexdump");
        memory_fill(address_of dump_arguments, 0, sizeof(dump_arguments));
        dump_arguments.limit = TEXT_UNSET;
        dump_arguments.address_base = 16;
        dump_arguments.address_width = 7;

        if (!file_take(address_of taking))
                return text_done(1);

        if ((taking.flags & FILE_FLAG('n')) &&
            !dump_number(file_option_value(address_of taking, 'n'),
                         address_of dump_arguments.limit))
                return text_refuse(file_option_value(address_of taking, 'n'),
                                   "invalid length", 1);

        if ((taking.flags & FILE_FLAG('s')) &&
            !dump_number(file_option_value(address_of taking, 's'),
                         address_of dump_arguments.skip))
                return text_refuse(file_option_value(address_of taking, 's'),
                                   "invalid skip", 1);

        dump_arguments.duplicates = (taking.flags & FILE_FLAG('v')) != 0;

        if (!dump_arguments.count)
                dump_add_integer(16, 2, 4, 4, false, true, false, true);

        if (dump_arguments.format[0].kind == DUMP_CANONICAL)
                dump_arguments.address_width = 8;

        return dump_run(taking.first);
}

// diff ------------------------------------------------------

#define DIFF_SPACE_NONE 0
#define DIFF_SPACE_CHANGE 4
#define DIFF_SPACE_ALL 5

#define DIFF_NORMAL 0
#define DIFF_UNIFIED 1

#define DIFF_LARGE (positive_max / 8)

static bool diff_icase;
static positive diff_space;
static bool diff_blank_lines;
static bool diff_brief;
static bool diff_recursive;
static bool diff_new_file;
static bool diff_new_file_left;
static bool diff_text;
static bool diff_identical;
static bool diff_trailing;
static bool diff_strip_cr;
static bool diff_tabs;
static positive diff_style;
static positive diff_context = 3;
static string_address diff_labels[2];
static positive diff_label_count;
static bool diff_style_seen;
static p8 address_to diff_switches;
static positive diff_switches_used;
static b32 diff_result;
static bool diff_titled;

typedef struct
{
        p8 address_to base;
        positive size;
        bool incomplete;
        positive lines;
        positive address_to at;
        positive prefix;
        positive count;
        b32 address_to class;
        p8 address_to changed;
        b32 address_to kept;
        positive address_to real;
        positive keeps;
        b64 modified_seconds;
        positive modified_nanoseconds;
        bool missing;
} diff_side;

static diff_side diff_files[2];

static fn diff_writer(address_any data, positive length)
{
        text_put(data, length ? length : string_length((string_address)data));
}

// Reading ---------------------------------------------------

static bool diff_slurp(diff_side address_to side, string_address path,
                       bool allow_missing)
{
        bipolar handle = 0;
        bool close_handle = false;

        side->base = null;
        side->size = 0;
        side->incomplete = false;
        side->missing = false;
        side->modified_seconds = 0;
        side->modified_nanoseconds = 0;

        if (path && !string_equals(path, "-"))
        {
                file_facts facts;

                if (file_look_at(path, address_of facts))
                {
                        side->modified_seconds = facts.modified.seconds;
                        side->modified_nanoseconds = facts.modified.nanoseconds;
                }

                handle = text_open_handle(path, FILE_READ, 0);

                if (handle < 0)
                {
                        if (allow_missing && handle == -ERROR_NO_ENTRY)
                        {
                                side->base = (p8 address_to)text_arena_take(16);
                                side->at = (positive address_to)text_arena_take(2 * sizeof(positive));

                                if (!side->base || !side->at)
                                        return false;

                                side->at[0] = 0;
                                side->missing = true;

                                return true;
                        }

                        text_error(path, file_reason(handle));
                        return false;
                }

                close_handle = true;
        }

        positive have = 0;
        bool read_failed;
        p8 address_to start = text_arena_read_all(
            (positive)handle, TEXT_READ_MAX, address_of have,
            address_of read_failed);

        if (close_handle)
                system_close(handle);

        if (!start)
        {
                if (read_failed)
                        text_error(path ? path : (string_address) "standard input",
                                   "Read error");
                return false;
        }

        if (diff_strip_cr && have > 1)
        {
                positive read = 0;
                positive write = 0;

                while (read < have)
                {
                        if (start[read] == '\r' && read + 1 < have &&
                            start[read + 1] == '\n')
                        {
                                read++;
                                continue;
                        }

                        start[write++] = start[read++];
                }

                have = write;
        }

        // One byte for the newline the file may not have, and one so the
        // scan below can look one past the end without care.
        p8 address_to tail = (p8 address_to)text_arena_take(16);

        if (!tail)
                return false;

        if (!start)
                start = tail;

        side->base = start;
        side->size = have;

        if (have && start[have - 1] != '\n')
        {
                start[have++] = '\n';
                side->incomplete = true;
                side->size = have;
        }

        positive lines = memory_count(start, have, '\n');

        side->lines = lines;
        side->at = (positive address_to)text_arena_take((lines + 2) * sizeof(positive));

        if (!side->at)
                return false;

        positive which = 0;
        positive from = 0;

        // The last byte is a newline by now, so every hunt below finds one.
        while (from < have)
        {
                p8 address_to cut = (p8 address_to)memory_first_of(start + from, '\n',
                                                                   have - from);

                side->at[which++] = from;
                from = (positive)(cut - start) + 1;
        }

        side->at[which] = have;

        return true;
}

// Lines -----------------------------------------------------

static p8 diff_fold(p8 value)
{
        if (diff_icase)
                return (p8)byte_to_lower(value);

        return value;
}

/*
        One line's worth of bytes, in the shape the ignore flags leave it.

        The walk stops at the newline every line in the buffer has, so the
        caller never has to know where a line ends.
*/
typedef struct
{
        p8 address_to at;
        p8 address_to stop;
        p8 held;
        positive column;
        positive tab_left;
        bool done;
} diff_scan;

static fn diff_scan_open(diff_scan address_to scan, p8 address_to line)
{
        scan->at = line;
        scan->stop = (p8 address_to)string_first_of_or_end(line, '\n');

        if (diff_trailing)
                while (scan->stop > line && byte_is_space(scan->stop[-1]))
                        scan->stop--;

        scan->done = false;
        scan->column = 0;
        scan->tab_left = 0;
}

static bool diff_scan_next(diff_scan address_to scan, p8 address_to out)
{
        if (scan->done)
                return false;

        if (scan->tab_left)
        {
                scan->tab_left--;
                scan->column++;
                address_to out = ' ';
                return true;
        }

        if (diff_space == DIFF_SPACE_ALL)
        {
                while (scan->at < scan->stop && byte_is_space(address_to scan->at))
                        scan->at++;

                if (scan->at == scan->stop)
                {
                        scan->done = true;
                        return false;
                }

                address_to out = diff_fold(address_to scan->at);
                scan->at++;

                return true;
        }

        if (diff_space == DIFF_SPACE_CHANGE)
        {
                if (scan->at == scan->stop)
                {
                        scan->done = true;
                        return false;
                }

                if (byte_is_space(address_to scan->at))
                {
                        while (scan->at < scan->stop && byte_is_space(address_to scan->at))
                                scan->at++;

                        if (scan->at == scan->stop)
                        {
                                scan->done = true;
                                return false;
                        }

                        address_to out = ' ';

                        return true;
                }

                address_to out = diff_fold(address_to scan->at);
                scan->at++;

                return true;
        }

        if (scan->at == scan->stop)
        {
                scan->done = true;
                return false;
        }

        if (diff_tabs && address_to scan->at == '\t')
        {
                positive spaces = 8 - scan->column % 8;

                scan->at++;
                scan->column++;
                scan->tab_left = spaces - 1;
                address_to out = ' ';
                return true;
        }

        address_to out = diff_fold(address_to scan->at);
        scan->at++;
        scan->column++;

        return true;
}

static p8 address_to diff_line(diff_side address_to side, bipolar middle)
{
        return side->base + side->at[(bipolar)side->prefix + middle];
}

// Without the newline every line in the buffer ends with.
static positive diff_line_length(diff_side address_to side, bipolar middle)
{
        positive line = side->prefix + (positive)middle;

        return side->at[line + 1] - side->at[line] - 1;
}

/*
        The last line of a file that has no newline of its own cannot be the
        same line as a complete one, however the bytes read -- which is the
        distinction GNU keeps by putting it in a bucket of its own. With -b or
        -w the trailing white space is already gone, and the distinction with
        it.
*/
static bool diff_stub(diff_side address_to side, bipolar middle)
{
        return side->incomplete && diff_space == DIFF_SPACE_NONE &&
               side->prefix + (positive)middle == side->lines - 1;
}

static PURE positive diff_hash(diff_side address_to side, bipolar middle)
{
        positive length = diff_line_length(side, middle);

        // The common exact-line path is already a bounded span. Short lines
        // stay inline; past the measured break-even the four-byte polynomial
        // floor removes three quarters of the dependent hash updates.
        if (length >= 24 && diff_space == DIFF_SPACE_NONE && !diff_trailing &&
            !diff_tabs && !diff_icase)
                return memory_hash_33(diff_line(side, middle), length) * 2 +
                       (diff_stub(side, middle) ? 1 : 0);

        diff_scan scan;
        positive value = 5381;
        p8 one;

        diff_scan_open(address_of scan, diff_line(side, middle));

        while (diff_scan_next(address_of scan, address_of one))
                value = value * 33 + one;

        return value * 2 + (diff_stub(side, middle) ? 1 : 0);
}

static PURE bool diff_same(diff_side address_to a, bipolar i, diff_side address_to b,
                      bipolar j)
{
        diff_scan left, right;
        p8 one, two;

        if (diff_stub(a, i) != diff_stub(b, j))
                return false;

        // With no whitespace normalization the lines remain exact bounded
        // spans; case folding only changes which block comparator proves it.
        if (diff_space == DIFF_SPACE_NONE && !diff_trailing && !diff_tabs)
        {
                positive length = diff_line_length(a, i);

                if (length != diff_line_length(b, j))
                        return false;

                return !(diff_icase
                              ? memory_compare_ascii_case(diff_line(a, i),
                                                          diff_line(b, j), length)
                              : memory_compare(diff_line(a, i), diff_line(b, j),
                                               length));
        }

        diff_scan_open(address_of left, diff_line(a, i));
        diff_scan_open(address_of right, diff_line(b, j));

        while (1)
        {
                bool more_left = diff_scan_next(address_of left, address_of one);
                bool more_right = diff_scan_next(address_of right, address_of two);

                if (!more_left || !more_right)
                        return more_left == more_right;

                if (one != two)
                        return false;
        }
}

// The classes, shared across both files so a number means the same thing
// on either side of the comparison.
typedef struct
{
        positive hash;
        b32 side;
        positive line;
} diff_class;

static diff_class address_to diff_classes;
static positive diff_class_count;
static b32 address_to diff_buckets;
static positive diff_bucket_count;

static bool diff_classify(diff_side address_to side, b32 which)
{
        side->class = (b32 address_to)text_arena_take((side->count + 1) * sizeof(b32));

        if (!side->class)
                return false;

        for (positive i = 0; i < side->count; i++)
        {
                positive hash = diff_hash(side, (bipolar)i);
                positive slot = hash & (diff_bucket_count - 1);
                b32 found = -1;

                while (diff_buckets[slot] >= 0)
                {
                        diff_class address_to have = diff_classes + diff_buckets[slot];

                        if (have->hash == hash &&
                            diff_same(diff_files + have->side, (bipolar)have->line, side, (bipolar)i))
                        {
                                found = diff_buckets[slot];
                                break;
                        }

                        slot = (slot + 1) & (diff_bucket_count - 1);
                }

                if (found < 0)
                {
                        found = (b32)diff_class_count;
                        diff_classes[diff_class_count].hash = hash;
                        diff_classes[diff_class_count].side = which;
                        diff_classes[diff_class_count].line = i;
                        diff_class_count++;
                        diff_buckets[slot] = found;
                }

                side->class[i] = found;
        }

        return true;
}

// Discarding ------------------------------------------------

// The loop this used to be is one compiler-visible highest-bit operation; the
// or with one keeps a zero from asking about a word with nothing set.
static positive diff_floor_log2(positive value)
{
        return top_bit_known(value | 1);
}

/*
        A line that matches nothing on the other side is a deletion whatever
        the matcher would have said, so it is taken out before the matcher
        runs. A line that matches a great many is only provisionally taken
        out, and put back unless it sits in the middle of a run of the first
        kind: this is GNU's discard_confusing_lines, and without it the
        matcher pairs up the wrong one of several identical lines.
*/
static bool diff_discard()
{
        positive total = diff_class_count + 1;
        positive address_to counts[2];
        p8 address_to marks[2];

        for (b32 f = 0; f < 2; f++)
        {
                counts[f] = (positive address_to)text_arena_take(total * sizeof(positive));

                if (!counts[f])
                        return false;

                memory_fill(counts[f], 0, total * sizeof(positive));

                for (positive i = 0; i < diff_files[f].count; i++)
                        counts[f][diff_files[f].class[i]]++;
        }

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                positive bound = side->count;

                marks[f] = (p8 address_to)text_arena_take(bound + 1);

                if (!marks[f])
                        return false;

                memory_fill(marks[f], 0, bound + 1);

                positive many = 5;

                if (bound >= 64)
                        many <<= (diff_floor_log2(bound) >> 1) - 3;

                for (positive i = 0; i < bound; i++)
                {
                        positive matches = counts[1 - f][side->class[i]];

                        if (!matches)
                                marks[f][i] = 1;
                        else if (matches > many)
                                marks[f][i] = 2;
                }
        }

        for (b32 f = 0; f < 2; f++)
        {
                positive bound = diff_files[f].count;
                p8 address_to mark = marks[f];

                for (positive i = 0; i < bound; i++)
                {
                        if (mark[i] == 2)
                        {
                                mark[i] = 0;
                                continue;
                        }

                        if (!mark[i])
                                continue;

                        positive provisional = 0;
                        positive j = i;

                        while (j < bound && mark[j])
                        {
                                if (mark[j] == 2)
                                        provisional++;

                                j++;
                        }

                        while (j > i && mark[j - 1] == 2)
                        {
                                j--;
                                mark[j] = 0;
                                provisional--;
                        }

                        positive length = j - i;

                        if ((length >> 2) < provisional)
                        {
                                while (j > i)
                                        if (mark[--j] == 2)
                                                mark[j] = 0;

                                continue;
                        }

                        positive least = length < 4
                                             ? 2
                                             : ((positive)1 << ((diff_floor_log2(length) >> 1) - 1)) + 1;
                        positive run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (mark[i + j] != 2)
                                {
                                        run = 0;
                                }
                                else if (least == ++run)
                                {
                                        j -= run;
                                }
                                else if (least < run)
                                {
                                        mark[i + j] = 0;
                                }
                        }

                        run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (j >= 8 && mark[i + j] == 1)
                                        break;

                                if (mark[i + j] == 2)
                                {
                                        run = 0;
                                        mark[i + j] = 0;
                                }
                                else if (!mark[i + j])
                                {
                                        run = 0;
                                }
                                else
                                {
                                        run++;
                                }

                                if (run == 3)
                                        break;
                        }

                        i += length - 1;
                        run = 0;

                        for (j = 0; j < length; j++)
                        {
                                if (j >= 8 && mark[i - j] == 1)
                                        break;

                                if (mark[i - j] == 2)
                                {
                                        run = 0;
                                        mark[i - j] = 0;
                                }
                                else if (!mark[i - j])
                                {
                                        run = 0;
                                }
                                else
                                {
                                        run++;
                                }

                                if (run == 3)
                                        break;
                        }
                }
        }

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                positive bound = side->count;

                side->kept = (b32 address_to)text_arena_take((bound + 1) * sizeof(b32));
                side->real = (positive address_to)text_arena_take((bound + 1) * sizeof(positive));

                if (!side->kept || !side->real)
                        return false;

                positive keeps = 0;

                for (positive i = 0; i < bound; i++)
                        if (!marks[f][i])
                        {
                                side->kept[keeps] = side->class[i];
                                side->real[keeps++] = i;
                        }
                        else
                        {
                                side->changed[i] = 1;
                        }

                side->keeps = keeps;
        }

        return true;
}

// The matcher -----------------------------------------------

static b32 address_to diff_forward;
static b32 address_to diff_backward;
static positive diff_middle;

static fn diff_meet(positive xoff, positive xlim, positive yoff, positive ylim,
                    positive address_to xmid, positive address_to ymid)
{
        b32 address_to xv = diff_files[0].kept;
        b32 address_to yv = diff_files[1].kept;
        bipolar low = (bipolar)xoff - (bipolar)ylim;
        bipolar high = (bipolar)xlim - (bipolar)yoff;
        bipolar front = (bipolar)xoff - (bipolar)yoff;
        bipolar back = (bipolar)xlim - (bipolar)ylim;
        bipolar front_low = front, front_high = front;
        bipolar back_low = back, back_high = back;
        bool odd = (front - back) & 1;
        positive shift = diff_middle;

        diff_forward[shift + front] = (b32)xoff;
        diff_backward[shift + back] = (b32)xlim;

        while (1)
        {
                if (front_low > low)
                        diff_forward[shift + --front_low - 1] = -1;
                else
                        front_low++;

                if (front_high < high)
                        diff_forward[shift + ++front_high + 1] = -1;
                else
                        front_high--;

                for (bipolar d = front_high; d >= front_low; d -= 2)
                {
                        bipolar lower = diff_forward[shift + d - 1];
                        bipolar upper = diff_forward[shift + d + 1];
                        bipolar x = lower < upper ? upper : lower + 1;
                        bipolar y = x - d;

                        while (x < (bipolar)xlim && y < (bipolar)ylim &&
                               xv[x] == yv[y])
                        {
                                x++;
                                y++;
                        }

                        diff_forward[shift + d] = (b32)x;

                        if (odd && back_low <= d && d <= back_high &&
                            diff_backward[shift + d] <= x)
                        {
                                address_to xmid = (positive)x;
                                address_to ymid = (positive)y;
                                return;
                        }
                }

                if (back_low > low)
                        diff_backward[shift + --back_low - 1] = 0x7fffffff;
                else
                        back_low++;

                if (back_high < high)
                        diff_backward[shift + ++back_high + 1] = 0x7fffffff;
                else
                        back_high--;

                for (bipolar d = back_high; d >= back_low; d -= 2)
                {
                        bipolar lower = diff_backward[shift + d - 1];
                        bipolar upper = diff_backward[shift + d + 1];
                        bipolar x = lower < upper ? lower : upper - 1;
                        bipolar y = x - d;

                        while ((bipolar)xoff < x && (bipolar)yoff < y &&
                               xv[x - 1] == yv[y - 1])
                        {
                                x--;
                                y--;
                        }

                        diff_backward[shift + d] = (b32)x;

                        if (!odd && front_low <= d && d <= front_high &&
                            x <= diff_forward[shift + d])
                        {
                                address_to xmid = (positive)x;
                                address_to ymid = (positive)y;
                                return;
                        }
                }
        }
}

// The high half is looped rather than recursed, which is what keeps the
// depth off a stack that has no room to grow.
static fn diff_compare(positive xoff, positive xlim, positive yoff, positive ylim)
{
        b32 address_to xv = diff_files[0].kept;
        b32 address_to yv = diff_files[1].kept;

        while (1)
        {
                while (xoff < xlim && yoff < ylim && xv[xoff] == yv[yoff])
                {
                        xoff++;
                        yoff++;
                }

                while (xoff < xlim && yoff < ylim && xv[xlim - 1] == yv[ylim - 1])
                {
                        xlim--;
                        ylim--;
                }

                if (xoff == xlim)
                {
                        while (yoff < ylim)
                                diff_files[1].changed[diff_files[1].real[yoff++]] = 1;

                        return;
                }

                if (yoff == ylim)
                {
                        while (xoff < xlim)
                                diff_files[0].changed[diff_files[0].real[xoff++]] = 1;

                        return;
                }

                positive xmid, ymid;

                diff_meet(xoff, xlim, yoff, ylim, address_of xmid, address_of ymid);
                diff_compare(xoff, xmid, yoff, ymid);

                xoff = xmid;
                yoff = ymid;
        }
}

/*
        Where a run of changed lines sits when either end of it could be the
        changed one.

        A minimal edit script is not unique, and this is the rule that makes
        ours the same one GNU prints: merge backwards into whatever came
        before, then slide forward as far as the lines allow, and finally
        pull back to line up with a run on the other side.
*/
static fn diff_shift()
{
        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                diff_side address_to other = diff_files + (1 - f);
                p8 address_to changed = side->changed;
                p8 address_to opposite = other->changed;
                b32 address_to equivalent = side->class;
                bipolar i = 0;
                bipolar j = 0;
                bipolar bound = (bipolar)side->count;

                while (1)
                {
                        while (i < bound && !changed[i])
                        {
                                while (opposite[j++])
                                        ;

                                i++;
                        }

                        if (i == bound)
                                break;

                        bipolar start = i;

                        while (changed[++i])
                                ;

                        while (opposite[j])
                                j++;

                        bipolar length, matching;

                        do
                        {
                                length = i - start;

                                while (start && equivalent[start - 1] == equivalent[i - 1])
                                {
                                        changed[--start] = 1;
                                        changed[--i] = 0;

                                        while (changed[start - 1])
                                                start--;

                                        while (opposite[--j])
                                                ;
                                }

                                matching = opposite[j - 1] ? i : bound;

                                while (i != bound && equivalent[start] == equivalent[i])
                                {
                                        changed[start++] = 0;
                                        changed[i++] = 1;

                                        while (changed[i])
                                                i++;

                                        while (opposite[++j])
                                                matching = i;
                                }
                        }
                        while (length != i - start);

                        while (matching < i)
                        {
                                changed[--start] = 1;
                                changed[--i] = 0;

                                while (opposite[--j])
                                        ;
                        }
                }
        }
}

// The script ------------------------------------------------

typedef struct
{
        positive line0;
        positive line1;
        positive deleted;
        positive inserted;
        bool ignore;
} diff_change;

static diff_change address_to diff_script;
static positive diff_script_count;

static bool diff_build()
{
        positive room = diff_files[0].count + diff_files[1].count + 2;

        diff_script = (diff_change address_to)text_arena_take(room * sizeof(diff_change));

        if (!diff_script)
                return false;

        diff_script_count = 0;

        positive i0 = 0, i1 = 0;
        positive len0 = diff_files[0].count, len1 = diff_files[1].count;
        p8 address_to c0 = diff_files[0].changed;
        p8 address_to c1 = diff_files[1].changed;

        while (i0 < len0 || i1 < len1)
        {
                if ((i0 < len0 && c0[i0]) || (i1 < len1 && c1[i1]))
                {
                        positive line0 = i0, line1 = i1;

                        while (i0 < len0 && c0[i0])
                                i0++;

                        while (i1 < len1 && c1[i1])
                                i1++;

                        diff_script[diff_script_count].line0 = line0;
                        diff_script[diff_script_count].line1 = line1;
                        diff_script[diff_script_count].deleted = i0 - line0;
                        diff_script[diff_script_count].inserted = i1 - line1;
                        diff_script[diff_script_count].ignore = false;
                        diff_script_count++;
                }

                i0++;
                i1++;
        }

        return true;
}

// Whether every line a change touches is one -B was told to skip over.
static PURE bool diff_trivial(positive from, positive to)
{
        if (!diff_blank_lines)
                return false;

        for (positive c = from; c < to; c++)
        {
                diff_change address_to one = diff_script + c;

                for (positive i = 0; i < one->deleted; i++)
                {
                        p8 address_to line = diff_line(diff_files + 0, (bipolar)(one->line0 + i));

                        if (address_to line != '\n')
                                return false;
                }

                for (positive i = 0; i < one->inserted; i++)
                {
                        p8 address_to line = diff_line(diff_files + 1, (bipolar)(one->line1 + i));

                        if (address_to line != '\n')
                                return false;
                }
        }

        return true;
}

static fn diff_mark_ignorable()
{
        for (positive c = 0; c < diff_script_count; c++)
                diff_script[c].ignore = diff_trivial(c, c + 1);
}

// Output ----------------------------------------------------

static fn diff_number(positive value)
{
        positive_to_string(text_put, value);
}

static fn diff_range(diff_side address_to side, bipolar first, bipolar last,
                     p8 separator)
{
        bipolar low = first + (bipolar)side->prefix + 1;
        bipolar high = last + (bipolar)side->prefix + 1;

        if (high > low)
        {
                diff_number((positive)low);
                text_put_character(separator);
                diff_number((positive)high);
        }
        else
        {
                diff_number((positive)high);
        }
}

static fn diff_unified_range(diff_side address_to side, bipolar first, bipolar last)
{
        bipolar low = first + (bipolar)side->prefix + 1;
        bipolar high = last + (bipolar)side->prefix + 1;

        if (high <= low)
        {
                diff_number((positive)high);

                if (high < low)
                        text_put_string(",0");

                return;
        }

        diff_number((positive)low);
        text_put_character(',');
        diff_number((positive)(high - low + 1));
}

static fn diff_put_line(diff_side address_to side, bipolar middle, string_address flag)
{
        p8 address_to line = diff_line(side, middle);
        positive where = (positive)((bipolar)side->prefix + middle);
        positive length = side->at[where + 1] - side->at[where];
        bool whole = !(side->incomplete && where == side->lines - 1);

        if (flag)
                text_put_string(flag);

        text_put(line, length - 1);

        if (whole)
                text_put_character('\n');
        else
                text_put_string("\n\\ No newline at end of file\n");
}

static fn diff_title(string_address left, string_address right)
{
        if (!diff_titled)
                return;

        text_put_string("diff");
        text_put(diff_switches, diff_switches_used);
        text_put_character(' ');
        text_put_string(left);
        text_put_character(' ');
        text_put_string(right);
        text_put_character('\n');
}

static fn diff_label(string_address mark, diff_side address_to side,
                     string_address name, string_address label)
{
        text_put_string(mark);
        text_put_character(' ');

        if (label)
        {
                text_put_string(label);
                text_put_character('\n');
                return;
        }

        text_put_string(name);
        text_put_character('\t');
        file_stamp(diff_writer, side->modified_seconds, side->modified_nanoseconds);
        text_put_character('\n');
}

static fn diff_normal_output()
{
        for (positive c = 0; c < diff_script_count; c++)
        {
                diff_change address_to one = diff_script + c;

                if (diff_trivial(c, c + 1))
                        continue;

                bipolar first0 = (bipolar)one->line0;
                bipolar last0 = (bipolar)(one->line0 + one->deleted) - 1;
                bipolar first1 = (bipolar)one->line1;
                bipolar last1 = (bipolar)(one->line1 + one->inserted) - 1;

                diff_range(diff_files + 0, first0, last0, ',');
                text_put_character(one->deleted && one->inserted
                                       ? 'c'
                                       : one->deleted ? 'd' : 'a');
                diff_range(diff_files + 1, first1, last1, ',');
                text_put_character('\n');

                for (positive i = 0; i < one->deleted; i++)
                        diff_put_line(diff_files + 0, first0 + (bipolar)i, "< ");

                if (one->deleted && one->inserted)
                        text_put_string("---\n");

                for (positive i = 0; i < one->inserted; i++)
                        diff_put_line(diff_files + 1, first1 + (bipolar)i, "> ");
        }
}

// A hunk runs on while the gap to the next change is smaller than the
// context it would print on either side of it.
static PURE positive diff_hunk_end(positive start)
{
        positive c = start;

        while (1)
        {
                positive top0 = diff_script[c].line0 + diff_script[c].deleted;

                if (c + 1 >= diff_script_count)
                        return c;

                positive threshold = diff_script[c + 1].ignore ? diff_context
                                                              : diff_context * 2 + 1;

                if (threshold <= diff_script[c + 1].line0 - top0)
                        return c;

                c++;
        }
}

static fn diff_unified_output(string_address left, string_address right)
{
        if (diff_blank_lines)
                diff_mark_ignorable();

        positive c = 0;
        bool headed = false;

        while (c < diff_script_count)
        {
                positive last = diff_hunk_end(c);
                bool anything = false;

                for (positive k = c; k <= last; k++)
                        if (!diff_script[k].ignore)
                                anything = true;

                if (!anything)
                {
                        c = last + 1;
                        continue;
                }

                bipolar first0 = (bipolar)diff_script[c].line0;
                bipolar first1 = (bipolar)diff_script[c].line1;
                bipolar last0 = (bipolar)(diff_script[last].line0 +
                                          diff_script[last].deleted) - 1;
                bipolar last1 = (bipolar)(diff_script[last].line1 +
                                          diff_script[last].inserted) - 1;

                /*
                        The context around a hunk comes out of the whole file,
                        not out of the middle the matcher was given, so these
                        walk back into the identical head and on into the
                        identical tail. That is where the negative index is
                        from and why none of this is unsigned.
                */
                bipolar floor0 = -(bipolar)diff_files[0].prefix;
                bipolar ceiling0 = (bipolar)diff_files[0].lines - (bipolar)diff_files[0].prefix;
                bipolar ceiling1 = (bipolar)diff_files[1].lines - (bipolar)diff_files[1].prefix;

                first0 -= (bipolar)diff_context;
                first1 -= (bipolar)diff_context;

                if (first0 < floor0)
                        first0 = floor0;

                if (first1 < floor0)
                        first1 = floor0;

                if (last0 < ceiling0 - (bipolar)diff_context)
                        last0 += (bipolar)diff_context;
                else
                        last0 = ceiling0 - 1;

                if (last1 < ceiling1 - (bipolar)diff_context)
                        last1 += (bipolar)diff_context;
                else
                        last1 = ceiling1 - 1;

                if (!headed)
                {
                        diff_title(left, right);
                        diff_label("---", diff_files + 0, left, diff_labels[0]);
                        diff_label("+++", diff_files + 1, right, diff_labels[1]);
                        headed = true;
                }

                text_put_string("@@ -");
                diff_unified_range(diff_files + 0, first0, last0);
                text_put_string(" +");
                diff_unified_range(diff_files + 1, first1, last1);
                text_put_string(" @@\n");

                positive at = c;
                bipolar i = first0;
                bipolar j = first1;

                while (i <= last0 || j <= last1)
                {
                        if (at > last || i < (bipolar)diff_script[at].line0)
                        {
                                diff_put_line(diff_files + 0, i++, " ");
                                j++;
                                continue;
                        }

                        for (positive k = 0; k < diff_script[at].deleted; k++)
                                diff_put_line(diff_files + 0, i++, "-");

                        for (positive k = 0; k < diff_script[at].inserted; k++)
                                diff_put_line(diff_files + 1, j++, "+");

                        at++;
                }

                c = last + 1;
        }
}

// Every one-line verdict diff gives is the same sentence around " and ",
// with a different head and tail.
static fn diff_announce(string_address head, string_address left,
                        string_address right, string_address tail)
{
        text_put_string(head);
        text_put_string(left);
        text_put_string(" and ");
        text_put_string(right);
        text_put_string(tail);
        text_flush();
}

static fn diff_identical_output(string_address left, string_address right)
{
        if (diff_identical)
                diff_announce("Files ", left, right, " are identical\n");
}

// One pair of files -----------------------------------------

static b32 diff_pair(string_address left, string_address right)
{
        diff_side address_to a = diff_files + 0;
        diff_side address_to b = diff_files + 1;

        memory_fill(a, 0, sizeof(diff_side));
        memory_fill(b, 0, sizeof(diff_side));

        if (!diff_slurp(a, left, diff_new_file || diff_new_file_left) ||
            !diff_slurp(b, right, diff_new_file))
                return 2;

        // Whether either file looks like something to diff by lines at all.
        if (!diff_text &&
            (memory_first_of(a->base, 0, a->size) ||
             memory_first_of(b->base, 0, b->size)))
        {
                // The newline a file did not end with is in its buffer all
                // the same, so a file with one and a file without have the
                // same size here; which of the two each is has to agree too.
                if (a->size == b->size && a->incomplete == b->incomplete &&
                    !memory_compare(a->base, b->base, a->size))
                {
                        diff_identical_output(left, right);
                        return 0;
                }

                diff_announce(diff_brief ? "Files " : "Binary files ", left,
                              right, " differ\n");

                return 1;
        }

        /*
                The identical head and tail are taken off before anything
                else, in bytes and then rounded back to whole lines, because
                that is where GNU takes them off and the matcher that runs
                after sees a different problem without it.
        */
        positive horizon = diff_style == DIFF_UNIFIED ? diff_context : 0;
        positive prefix = 0;
        positive shortest = a->size < b->size ? a->size : b->size;
        positive bytes = memory_common_prefix(a->base, b->base, shortest);

        /*
                The newline a file did not have is in the buffer anyway, and
                a head that runs past where one file really ended is a head
                that includes a byte only one of them wrote. Backing off one
                byte here is what keeps an incomplete last line from being
                trimmed away as identical to a complete one.
        */
        {
                positive real_a = a->size - (a->incomplete ? 1 : 0);
                positive real_b = b->size - (b->incomplete ? 1 : 0);

                if ((real_a < bytes) != (real_b < bytes))
                        bytes--;
        }

        {
                p8 address_to line_end =
                    (p8 address_to)memory_last_of(a->base, '\n', bytes);

                bytes = line_end ? (positive)(line_end - a->base) + 1 : 0;
        }

        while (prefix < a->lines && a->at[prefix] < bytes)
                prefix++;

        /*
                A context style keeps back as many lines as it is going to
                print around a hunk, because a line inside the identical head
                is a line the matcher is not allowed to move a run of changes
                into. Without it -u and the plain format disagree about which
                of two identical lines is the changed one, and only in the
                cases where a hunk reaches the edge of what was trimmed.
        */
        prefix = prefix > horizon ? prefix - horizon : 0;
        bytes = a->at[prefix];

        positive suffix = 0;

        if (a->incomplete == b->incomplete)
        {
                positive tail = 0;

                while (tail < a->size - bytes && tail < b->size - bytes &&
                       a->base[a->size - 1 - tail] == b->base[b->size - 1 - tail])
                        tail++;

                positive stop_a = a->size - tail;
                positive stop_b = b->size - tail;

                // The identical tail has to begin a line in both files, and
                // it is the one that does not that decides: a tail that
                // starts inside a line on either side costs a whole line.
                positive drop = horizon +
                                !((!stop_a || a->base[stop_a - 1] == '\n') &&
                                  (!stop_b || b->base[stop_b - 1] == '\n'));

                while (drop && stop_a != a->size)
                {
                        p8 address_to line_end = (p8 address_to)memory_first_of(
                            a->base + stop_a, '\n', a->size - stop_a);

                        drop--;
                        stop_a = line_end ? (positive)(line_end - a->base) + 1
                                          : a->size;
                }

                while (suffix < a->lines - prefix &&
                       a->at[a->lines - 1 - suffix] >= stop_a)
                        suffix++;

                if (suffix > b->lines - prefix)
                        suffix = b->lines - prefix;
        }

        a->prefix = prefix;
        b->prefix = prefix;
        a->count = a->lines - prefix - suffix;
        b->count = b->lines - prefix - suffix;

        for (b32 f = 0; f < 2; f++)
        {
                diff_side address_to side = diff_files + f;
                p8 address_to room = (p8 address_to)text_arena_take(side->count + 4);

                if (!room)
                        return 2;

                memory_fill(room, 0, side->count + 4);
                side->changed = room + 1;
        }

        positive total = a->count + b->count + 4;

        diff_bucket_count = 8;

        while (diff_bucket_count < total * 2)
                diff_bucket_count <<= 1;

        diff_classes = (diff_class address_to)text_arena_take(total * sizeof(diff_class));
        diff_buckets = (b32 address_to)text_arena_take(diff_bucket_count * sizeof(b32));

        if (!diff_classes || !diff_buckets)
                return 2;

        memory_fill(diff_buckets, (b8)-1,
                    diff_bucket_count * sizeof(diff_buckets[0]));

        diff_class_count = 0;

        if (!diff_classify(a, 0) || !diff_classify(b, 1))
                return 2;

        if (!diff_discard())
                return 2;

        positive diagonals = a->keeps + b->keeps + 3;

        diff_forward = (b32 address_to)text_arena_take((diagonals + 4) * sizeof(b32));
        diff_backward = (b32 address_to)text_arena_take((diagonals + 4) * sizeof(b32));

        if (!diff_forward || !diff_backward)
                return 2;

        diff_middle = b->keeps + 2;

        diff_compare(0, a->keeps, 0, b->keeps);
        diff_shift();

        if (!diff_build())
                return 2;

        bool changes = false;

        if (diff_blank_lines)
        {
                for (positive c = 0; c < diff_script_count; c++)
                        if (!diff_trivial(c, c + 1))
                                changes = true;
        }
        else
        {
                changes = diff_script_count != 0;
        }

        if (!changes)
        {
                diff_identical_output(left, right);
                return 0;
        }

        if (diff_brief)
        {
                diff_announce("Files ", left, right, " differ\n");

                return 1;
        }

        if (diff_style == DIFF_UNIFIED)
        {
                diff_unified_output(left, right);
        }
        else
        {
                diff_title(left, right);
                diff_normal_output();
        }

        text_flush();

        return 1;
}

// Directories -----------------------------------------------

/*
        The names in one directory, with the bytes taken from the arena.

        A walk that is going to recurse cannot keep its names in a buffer of
        its own, because the level below would write over them and the level
        above would go on comparing whatever landed there.
*/
typedef struct
{
        string_address address_to at;
        positive count;
        positive room;
} diff_names;

static bool diff_name_add(diff_names address_to names, string_address value)
{
        if (!array_arena_reserve(names->at, names->room, names->count,
                                 names->count + 1, 32, text_arena_grow))
                return false;

        names->at[names->count++] = value;
        return true;
}

static bool diff_names_sort(diff_names address_to names)
{
        if (names->count < 2)
                return true;

        string_address address_to spare =
            (string_address address_to)text_arena_take(
                names->count * sizeof(string_address));

        if (!spare)
                return false;

        names->at = array_merge_sort(names->at, spare, names->count,
                                     string_compare);
        return true;
}

static string_address diff_path(string_address directory, string_address name)
{
        positive head = string_length(directory);
        positive tail = string_length(name);
        positive slash = head && directory[head - 1] != '/';

        if (tail > positive_max - slash - 1 ||
            head > positive_max - tail - slash - 1)
                return null;

        positive room = head + tail + slash + 1;
        p8 address_to joined = (p8 address_to)text_arena_take(room);

        if (!joined)
                return null;

        path_join(joined, room, directory, name);
        return (string_address)joined;
}

static bool diff_gather(string_address path, diff_names address_to names,
                        bool allow_missing)
{
        file_walk walk;

        names->at = null;
        names->count = 0;
        names->room = 0;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                if (allow_missing)
                        return true;

                text_error(path, "No such file or directory");
                return false;
        }

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                positive length = string_length(entry->d_name);
                p8 address_to at = (p8 address_to)text_arena_take(length + 1);

                if (!at)
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                memory_copy_apart(at, entry->d_name, length + 1);

                if (!diff_name_add(names, (string_address)at))
                {
                        file_walk_close(address_of walk);
                        return false;
                }
        }

        file_walk_close(address_of walk);

        return diff_names_sort(names);
}

static b32 diff_walk(string_address left, string_address right, positive depth);

/*
        A name present in one directory only. With the new-file flags it is
        compared against the nothing on the other side; otherwise it is
        announced. A path that could not be made says so through failed,
        because that must stop the walk while an ordinary difference must
        not.
*/
static b32 diff_one_sided(string_address left, string_address right,
                          string_address inside, string_address name,
                          bool compare, positive depth,
                          bool address_to failed)
{
        if (compare)
        {
                string_address only_left = diff_path(left, name);
                string_address only_right = diff_path(right, name);

                if (!only_left || !only_right)
                {
                        address_to failed = true;
                        return 2;
                }

                return diff_walk(only_left, only_right, depth + 1);
        }

        text_put_string("Only in ");
        text_put_string(inside);
        text_put_string(": ");
        text_put_string(name);
        text_put_character('\n');
        text_flush();

        return 1;
}

static b32 diff_directories(string_address left, string_address right, positive depth)
{
        if (depth >= FILE_MAX_DEPTH)
                return 2;

        diff_names names[2];

        if (!diff_gather(left, names + 0, diff_new_file || diff_new_file_left) ||
            !diff_gather(right, names + 1, diff_new_file))
                return 2;

        positive i = 0, j = 0;
        b32 worst = 0;

        while (i < names[0].count || j < names[1].count)
        {
                /* The two gathered name tables have to survive the whole
                   directory, but paths and file-diff workspaces belong only
                   to this child. Rewind them after the child returns so a
                   wide tree costs its maximum file, not the sum of every
                   file visited before it. */
                positive child_mark = text_arena_used;
                b32 order = i >= names[0].count
                                ? 1
                                : j >= names[1].count
                                      ? -1
                                      : (b32)string_compare(names[0].at[i],
                                                            names[1].at[j]);

                if (order)
                {
                        bool failed = false;
                        b32 one = order < 0
                                      ? diff_one_sided(left, right, left,
                                                       names[0].at[i],
                                                       diff_new_file, depth,
                                                       address_of failed)
                                      : diff_one_sided(left, right, right,
                                                       names[1].at[j],
                                                       diff_new_file ||
                                                           diff_new_file_left,
                                                       depth,
                                                       address_of failed);

                        if (failed)
                                return 2;

                        if (worst < one)
                                worst = one;

                        text_arena_used = child_mark;

                        if (order < 0)
                                i++;
                        else
                                j++;

                        continue;
                }

                string_address one_left = diff_path(left, names[0].at[i]);
                string_address one_right = diff_path(right, names[1].at[j]);

                if (!one_left || !one_right)
                        return 2;

                bool left_directory = file_is_directory_through(one_left);
                bool right_directory = file_is_directory_through(one_right);

                if (left_directory && right_directory && !diff_recursive)
                {
                        diff_announce("Common subdirectories: ", one_left,
                                      one_right, "\n");
                }
                else
                {
                        b32 one = diff_walk(one_left, one_right, depth + 1);

                        if (worst < one)
                                worst = one;
                }

                text_arena_used = child_mark;

                i++;
                j++;
        }

        return worst;
}

static b32 diff_walk(string_address left, string_address right, positive depth)
{
        bool left_here = file_exists(AT_FDCWD, left);
        bool right_here = file_exists(AT_FDCWD, right);
        bool left_directory = left_here && file_is_directory_through(left);
        bool right_directory = right_here && file_is_directory_through(right);

        /*
                Two names for the same regular inode cannot differ. Avoiding
                the pair of complete reads is especially important here:
                diff keeps both inputs for its line algorithm, so an otherwise
                trivial same-file comparison could consume the entire arena.
        */
        if (left_here && right_here && !left_directory && !right_directory)
        {
                file_facts left_facts;
                file_facts right_facts;

                if (file_look_at(left, address_of left_facts) &&
                    file_look_at(right, address_of right_facts) &&
                    (left_facts.mode & MODE_FORMAT) == MODE_FILE &&
                    (right_facts.mode & MODE_FORMAT) == MODE_FILE &&
                    file_same_identity(address_of left_facts,
                                       address_of right_facts))
                {
                        diff_identical_output(left, right);
                        return 0;
                }
        }

        bool absent_directory_is_empty =
            !(left_here && right_here) &&
            (diff_new_file || (diff_new_file_left && !left_here && right_here));

        if ((left_directory || right_directory) &&
            (left_directory == right_directory || absent_directory_is_empty))
                return diff_directories(left, right, depth);

        if (left_directory != right_directory && left_here && right_here)
        {
                text_put_string("File ");
                text_put_string(left);
                text_put_string(" is a ");
                text_put_string(left_directory ? "directory" : "regular file");
                text_put_string(" while file ");
                text_put_string(right);
                text_put_string(" is a ");
                text_put_string(right_directory ? "directory" : "regular file");
                text_put_character('\n');
                text_flush();

                return 1;
        }

        bool titled = diff_titled;
        positive pair_mark = text_arena_used;

        diff_titled = depth > 0;

        b32 one = diff_pair(left, right);

        diff_titled = titled;
        text_arena_used = pair_mark;

        return one;
}

static const file_long diff_longs[] = {
    {(string_address) "normal", 'z'},
    {(string_address) "unified", 'v'},
    {(string_address) "brief", 'q'},
    {(string_address) "report-identical-files", 's'},
    {(string_address) "recursive", 'r'},
    {(string_address) "new-file", 'N'},
    {(string_address) "unidirectional-new-file", 'O'},
    {(string_address) "no-ignore-file-name-case", 'J'},
    {(string_address) "ignore-case", 'i'},
    {(string_address) "ignore-tab-expansion", 'E'},
    {(string_address) "ignore-all-space", 'w'},
    {(string_address) "ignore-space-change", 'b'},
    {(string_address) "ignore-trailing-space", 'Z'},
    {(string_address) "ignore-blank-lines", 'B'},
    {(string_address) "text", 'a'},
    {(string_address) "strip-trailing-cr", 'R'},
    {(string_address) "speed-large-files", 'h'},
    {(string_address) "label", 'L'},
    {null, 0},
};

static bool diff_context_set(string_address value)
{
        positive context;
        string_address at = value;

        if (!value)
        {
                diff_context = 3;
                return true;
        }

        if (!string_digits_checked(address_of at, 10, address_of context) || string_get(at) ||
            context > (positive_max - 1) / 2)
        {
                text_error(value, "invalid context length");
                return false;
        }

        diff_context = context;
        return true;
}

// Labels, context values and output styles need arrival order; one flag word
// and one value per letter cannot represent any of those three contracts.
static bool diff_option_seen(p8 letter, string_address value)
{
        if (letter == 'L')
        {
                if (diff_label_count >= 2)
                {
                        text_error(value, "too many file label options");
                        return false;
                }

                diff_labels[diff_label_count++] = value;
                return true;
        }

        if (letter == 'u' || letter == 'U' || letter == 'v' || letter == 'z')
        {
                positive style = letter == 'z' ? DIFF_NORMAL : DIFF_UNIFIED;

                if (diff_style_seen && diff_style != style)
                {
                        text_error(null, "conflicting output style options");
                        return false;
                }

                diff_style = style;
                diff_style_seen = true;

                if (style == DIFF_UNIFIED && (letter == 'U' || letter == 'v'))
                        return diff_context_set(value);

                return true;
        }

        return true;
}

static b32 tools_diff(void)
{
        file_taking taking = {
            .program = (string_address) "diff",
            .allowed = (string_address) "BELNUZabiqrsuw",
            .valued = (string_address) "LU",
            .optional = (string_address) "v",
            .longs = diff_longs,
            .seen = diff_option_seen,
        };

        text_begin("diff");

        diff_brief = false;
        diff_style = DIFF_NORMAL;
        diff_context = 3;
        diff_labels[0] = diff_labels[1] = null;
        diff_label_count = 0;
        diff_style_seen = false;
        diff_switches_used = 0;
        diff_titled = false;
        text_arena_used = 0;

        if (!file_take(address_of taking))
                return text_done(2);

        positive flags = taking.flags;
        b32 first = (b32)taking.first;

        positive switches_room = 1;

        for (positive i = 1; i < taking.first; i++)
                switches_room += string_length(program_argument((b32)i)) + 1;

        diff_switches = (p8 address_to)text_arena_take(switches_room);

        if (!diff_switches)
                return text_done(2);

        diff_icase = (flags & FILE_FLAG('i')) != 0;
        diff_blank_lines = (flags & FILE_FLAG('B')) != 0;
        diff_recursive = (flags & FILE_FLAG('r')) != 0;
        diff_new_file = (flags & FILE_FLAG('N')) != 0;
        diff_new_file_left = (flags & FILE_FLAG('O')) != 0;
        diff_text = (flags & FILE_FLAG('a')) != 0;
        diff_identical = (flags & FILE_FLAG('s')) != 0;
        diff_trailing = (flags & FILE_FLAG('Z')) != 0;
        diff_strip_cr = (flags & FILE_FLAG('R')) != 0;
        diff_tabs = (flags & FILE_FLAG('E')) != 0;
        diff_brief = (flags & FILE_FLAG('q')) != 0;
        diff_space = (flags & FILE_FLAG('w'))   ? DIFF_SPACE_ALL
                     : (flags & FILE_FLAG('b')) ? DIFF_SPACE_CHANGE
                                                : DIFF_SPACE_NONE;

        // What the header of a piece of a recursive diff repeats is the
        // options as they were typed, not a canonical spelling -- and a label
        // written as its own word is not one of them.
        for (positive i = 1; i < taking.first; i++)
        {
                string_address word = program_argument((b32)i);
                positive length = string_length(word);

                if (!string_is(word, '-') || length < 2 ||
                    (string_is(word + 1, '-') && length == 2))
                        continue;

                diff_switches[diff_switches_used++] = ' ';
                memory_copy_apart(diff_switches + diff_switches_used, word, length);
                diff_switches_used += length;

                if (word[length - 1] == 'U' && i + 1 < taking.first)
                {
                        string_address context = program_argument((b32)(i + 1));
                        positive context_length = string_length(context);

                        diff_switches[diff_switches_used++] = ' ';
                        memory_copy_apart(diff_switches + diff_switches_used,
                                         context, context_length);
                        diff_switches_used += context_length;
                }
        }

        if (text_argument_count - first != 2)
        {
                text_error(null, "missing operand");
                return text_done(2);
        }

        string_address left = program_argument(first);
        string_address right = program_argument(first + 1);
        string_address joined;

        // diff dir file and diff file dir both mean the same file inside the
        // directory, which is the one place the two names are not the pair.
        if (file_is_directory_through(left) && !file_is_directory_through(right))
        {
                joined = diff_path(left, file_last_component(right));

                if (!joined)
                        return text_done(2);

                left = joined;
        }
        else if (!file_is_directory_through(left) && file_is_directory_through(right))
        {
                joined = diff_path(right, file_last_component(left));

                if (!joined)
                        return text_done(2);

                right = joined;
        }

        if (string_equals(left, "-") && string_equals(right, "-"))
        {
                diff_identical_output(left, right);
                diff_result = 0;
        }
        else
        {
                diff_result = diff_walk(left, right, 0);
        }

        text_done(diff_result);

        // A diagnostic utility distinguishes a difference (1) from being
        // unable to report one (2). A failed final buffered flush is the
        // latter even when the comparison itself completed.
        return text_out_failed ? 2 : diff_result;
}

// ps --------------------------------------------------------

#define PS_FIELD_PID 0
#define PS_FIELD_PPID 1
#define PS_FIELD_USER 2
#define PS_FIELD_COMM 3
#define PS_FIELD_ARGS 4
#define PS_FIELD_STAT 5
#define PS_FIELD_TIME 6
#define PS_FIELD_ETIME 7
#define PS_FIELD_RSS 8
#define PS_FIELD_VSZ 9
#define PS_FIELD_TTY 10
#define PS_FIELD_UID 11
#define PS_FIELD_CPU 12
#define PS_FIELD_STIME 13
#define PS_FIELD_SID 14
#define PS_FIELD_PGID 15
#define PS_FIELD_NLWP 16
#define PS_FIELD_ETIMES 17
#define PS_FIELD_COUNT 18

typedef struct
{
        string_address name;
        string_address header;
        positive width;
        bool right;
} ps_column;

static ps_column ps_columns[PS_FIELD_COUNT] = {
    {"pid", "PID", 7, true},        {"ppid", "PPID", 7, true},
    {"user", "USER", 8, false},     {"comm", "COMMAND", 15, false},
    {"args", "COMMAND", 27, false}, {"stat", "STAT", 4, false},
    {"time", "TIME", 8, true},      {"etime", "ELAPSED", 11, true},
    {"rss", "RSS", 5, true},        {"vsz", "VSZ", 6, true},
    {"tty", "TT", 2, false},        {"uid", "UID", 5, true},
    {"c", "C", 2, true},            {"stime", "STIME", 5, false},
    {"sid", "SID", 7, true},         {"pgid", "PGID", 7, true},
    {"nlwp", "NLWP", 4, true},       {"etimes", "ELAPSED", 7, true}};

typedef struct
{
        p8 state[8];
        string_address args;
        string_address user;
} ps_detail;

static system_snapshot ps_snapshot;
static positive ps_now;
static positive ps_own_tty;
static positive ps_own_uid;
static positive ps_wall;

// The name behind a numeric user id. file.c already reads /etc/passwd and
// remembers the last answer, so ps only keeps a copy that lives as long as
// the arena; the fallback for an unknown id is the number spelled out.
static string_address ps_name_of(positive uid)
{
        p8 name[FILE_NAME_MAX];
        positive length;

        if (file_user_name(uid, name, FILE_NAME_MAX))
                length = string_length(name);
        else
                length = positive_into(name, uid);

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return null;

        memory_copy_apart_end(made, name, length);
        return (string_address)made;
}

static string_address ps_arguments(struct snapshot_process address_to process)
{
        p8 path[64];

        system_process_path(path, process->pid, null, "cmdline");

        positive got = 0;
        bipolar handle = text_open_handle(path, FILE_READ, 0);
        p8 address_to command = handle < 0
            ? null
            : text_arena_read_all((positive)handle, 256, address_of got, null);

        if (handle >= 0)
                system_close(handle);

        if (command && got)
        {
                for (positive i = 0; i < got; i++)
                        command[i] = command[i] ? command[i] : ' ';

                while (got && command[got - 1] == ' ')
                        got--;

                command[got] = end;
                return (string_address)command;
        }

        positive length = string_length(process->command);
        p8 address_to fallback =
            (p8 address_to)text_arena_take(length + 3);

        if (!fallback)
                return null;

        fallback[0] = '[';
        memory_copy_apart(fallback + 1, process->command, length);
        fallback[1 + length] = ']';
        fallback[2 + length] = end;
        return (string_address)fallback;
}

static string_address ps_state(ps_detail address_to detail,
                               struct snapshot_process address_to process)
{
        positive mark = 1;

        detail->state[0] = (p8)process->state;
        if (process->nice < 0)
                detail->state[mark++] = '<';
        else if (process->nice > 0)
                detail->state[mark++] = 'N';
        if (process->session == process->pid)
                detail->state[mark++] = 's';
        if (process->threads > 1)
                detail->state[mark++] = 'l';
        if (process->tpgid == (int)process->pgrp)
                detail->state[mark++] = '+';
        detail->state[mark] = end;
        return (string_address)detail->state;
}

/*
        A field is drawn into a small buffer first, because a column is padded
        by how wide what it drew turned out to be and the shared output buffer
        can empty itself between one byte and the next.
*/
static p8 address_to ps_room;
static positive ps_room_used;
static positive ps_room_size;
static bool ps_failed;

static HOT bool ps_room_add(positive extra)
{
        if (ps_room_used == positive_max ||
            extra > positive_max - ps_room_used - 1)
                goto failed;

        positive wanted = ps_room_used + extra + 1;

        if (wanted > ps_room_size &&
            !text_arena_grow(address_of ps_room, address_of ps_room_size,
                             ps_room_used, wanted, 1, 64))
                goto failed;

        return true;

failed:
        ps_failed = true;
        return false;
}

static fn ps_byte(p8 value)
{
        if (ps_room_add(1))
                ps_room[ps_room_used++] = value;
}

static fn ps_bytes(address_any value, positive length)
{
        if (!ps_room_add(length))
                return;

        memory_copy_apart(ps_room + ps_room_used, value, length);
        ps_room_used += length;
}

static fn ps_text(string_address value)
{
        if (value)
                ps_bytes(value, string_length(value));
}

static fn ps_digits(positive value)
{
        if (ps_room_add(20))
        {
                ps_room_used += positive_into(ps_room + ps_room_used, value);
                return;
        }

        p8 have[24];
        positive length = positive_into(have, value);

        ps_bytes(have, length);
}

/*
        D-HH:MM:SS, the way procps spells both TIME and ELAPSED: the day
        only when there is one, so the hours never climb past 23. The two
        columns differ in one place, which is that ELAPSED under an hour is
        MM:SS and TIME is never shorter than HH:MM:SS.
*/
static fn ps_put_clock(positive seconds, bool hours_always)
{
        positive days = seconds / 86400;
        positive rest = seconds % 86400;

        if (days)
        {
                ps_digits(days);
                ps_byte('-');
        }

        if (days || hours_always || rest >= 3600)
        {
                file_two(ps_bytes, rest / 3600);
                ps_byte(':');
        }

        file_two(ps_bytes, (rest / 60) % 60);
        ps_byte(':');
        file_two(ps_bytes, rest % 60);
}

static fn ps_put_time(positive nanoseconds)
{
        ps_put_clock(nanoseconds / SYSTEM_NANOSECONDS, true);
}

static fn ps_put_elapsed(positive seconds)
{
        ps_put_clock(seconds, false);
}

static fn ps_put_tty(positive tty)
{
        if (!tty)
        {
                ps_byte('?');
                return;
        }

        positive major = (tty >> 8) & 0xfff;
        positive minor = (tty & 0xff) | ((tty >> 12) & 0xfff00);
        string_address name = "tty";

        /*
                The kernel's fixed assignments rather than a walk of /dev:
                eight majors of pseudo terminals numbered straight through,
                the serial ports sharing the console major from minor 64
                up, and the hypervisor and USB drivers a process is likely
                to be sitting on. Anything else is a tty of its minor.
        */
        if (major >= 136 && major <= 143)
        {
                name = "pts/";
                minor += (major - 136) * 256;
        }
        else if (major == 4 && minor >= 64)
        {
                name = "ttyS";
                minor -= 64;
        }
        else if (major == 229)
        {
                name = "hvc";
        }
        else if (major == 188)
        {
                name = "ttyUSB";
        }

        ps_text(name);
        ps_digits(minor);
}

static fn ps_draw(struct snapshot_process address_to process,
                  ps_detail address_to detail, positive field)
{
        ps_room_used = 0;

        switch (field)
        {
        case PS_FIELD_PID: ps_digits(process->pid); break;
        case PS_FIELD_PPID: ps_digits(process->ppid); break;
        case PS_FIELD_USER:
                if (!detail->user &&
                    !(detail->user = ps_name_of(process->uid)))
                        ps_failed = true;
                ps_text(detail->user);
                break;
        case PS_FIELD_UID: ps_digits(process->uid); break;
        case PS_FIELD_COMM: ps_text(process->command); break;
        case PS_FIELD_ARGS:
                if (!detail->args &&
                    !(detail->args = ps_arguments(process)))
                        ps_failed = true;
                ps_text(detail->args);
                break;
        case PS_FIELD_STAT:
                ps_text(detail->state[0] ? (string_address)detail->state
                                         : ps_state(detail, process));
                break;
        case PS_FIELD_TIME:
                ps_put_time(system_saturating_add(process->user_ns,
                                                  process->system_ns));
                break;
        case PS_FIELD_ETIME:
        {
                positive began = process->start_ns / SYSTEM_NANOSECONDS;

                ps_put_elapsed(ps_now > began ? ps_now - began : 0);
                break;
        }
        case PS_FIELD_RSS: ps_digits(process->resident_bytes / 1024); break;
        case PS_FIELD_VSZ: ps_digits(process->virtual_bytes / 1024); break;
        case PS_FIELD_TTY: ps_put_tty((positive)process->tty); break;
        case PS_FIELD_CPU:
        {
                positive began = process->start_ns / SYSTEM_NANOSECONDS;
                positive lived = ps_now > began
                                     ? ps_now - began
                                     : 0;

                ps_digits(lived ? system_saturating_add(process->user_ns,
                                                        process->system_ns) /
                                      SYSTEM_NANOSECONDS * 100 / lived
                                : 0);
                break;
        }
        case PS_FIELD_STIME:
        {
                b64 began = (b64)ps_wall - (b64)ps_now +
                            (b64)(process->start_ns / SYSTEM_NANOSECONDS);
                b64 year, year_now;
                positive month, day, hour, minute, second;
                positive month_now, day_now, hour_now, minute_now, second_now;
                file_split_moment(began, address_of year, address_of month,
                                  address_of day, address_of hour,
                                  address_of minute, address_of second);
                file_split_moment((b64)ps_wall, address_of year_now, address_of month_now,
                                  address_of day_now, address_of hour_now,
                                  address_of minute_now, address_of second_now);

                if (year == year_now && month == month_now && day == day_now)
                {
                        file_two(ps_bytes, hour);
                        ps_byte(':');
                        file_two(ps_bytes, minute);
                }
                else if (year == year_now)
                {
                        file_month_short(ps_bytes, month);
                        file_two(ps_bytes, day);
                }
                else
                {
                        ps_digits((positive)year);
                }

                break;
        }
        case PS_FIELD_SID: ps_digits(process->session); break;
        case PS_FIELD_PGID: ps_digits(process->pgrp); break;
        case PS_FIELD_NLWP: ps_digits(process->threads); break;
        case PS_FIELD_ETIMES:
        {
                positive began = process->start_ns / SYSTEM_NANOSECONDS;

                ps_digits(ps_now > began ? ps_now - began : 0);
                break;
        }
        default: break;
        }

        if (ps_room_add(0))
                ps_room[ps_room_used] = end;
}

static fn ps_column_out(struct snapshot_process address_to process,
                        ps_detail address_to detail, positive field,
                        positive width, bool last)
{
        ps_draw(process, detail, field);

        // A column that something follows is exactly as wide as it says,
        // which is where the reference cuts a long command line off.
        if (!last && ps_room_used > width)
                ps_room_used = width;

        writer_field(text_put, ps_room, ps_room_used,
                     !ps_columns[field].right && last ? ps_room_used : width,
                     ' ', !ps_columns[field].right);

        if (!last)
                text_put_character(' ');
}

typedef struct
{
        positive field;
        string_address header;
        bool custom_header;
} ps_selected;

/*
        A column is as wide as its table entry or its heading, whichever is
        longer, and the heading is the -o one when the caller wrote one. The
        header line and every row under it have to agree on both, so both
        ask here; a caller that wants only the width passes no header.
*/
static positive ps_column_width(ps_selected address_to selected,
                                string_address address_to header)
{
        ps_column address_to column = ps_columns + selected->field;
        string_address heading = selected->custom_header ? selected->header
                                                         : column->header;
        positive length = heading ? string_length(heading) : 0;

        if (header)
                address_to header = heading;

        return length > column->width ? length : column->width;
}

static bool ps_field_add(ps_selected address_to address_to fields,
                         positive address_to count, positive address_to room,
                         positive value, string_address header,
                         bool custom_header)
{
        if (!text_arena_grow(fields, room, address_to count,
                             address_to count + 1, sizeof(ps_selected), 16))
                return false;

        (address_to fields)[address_to count].field = value;
        (address_to fields)[address_to count].header = header;
        (address_to fields)[address_to count].custom_header = custom_header;
        address_to count += 1;
        return true;
}

static bool ps_value_add(positive address_to address_to values,
                         positive address_to count, positive address_to room,
                         positive value)
{
        if (!text_arena_grow(values, room, address_to count,
                             address_to count + 1, sizeof(positive), 16))
                return false;

        (address_to values)[address_to count] = value;
        address_to count += 1;
        return true;
}

static bool ps_value_has(positive address_to values, positive count,
                         positive value)
{
        for (positive i = 0; i < count; i++)
                if (values[i] == value)
                        return true;

        return false;
}

typedef struct
{
        string_address at;
        string_address from;
        positive length;
} ps_list_cursor;

/* A comma-or-space separated span. The caller supplies the bytes that end
   its kind of item because a format name also stops at =, while a command
   name may contain one. A separator-only tail is returned once as an empty
   span: three readers ignore it, while --sort deliberately refuses it. */
static bool ps_list_next(ps_list_cursor address_to list,
                         string_address stops)
{
        if (!string_get(list->at))
                return false;

        list->at += string_span_of_set(list->at, ", ");
        list->from = list->at;
        list->length = string_span_without_set(list->at, stops);
        list->at += list->length;
        return true;
}

static bool ps_pid_list(string_address list,
                        positive address_to address_to values,
                        positive address_to count, positive address_to room,
                        bool duplicate_within_operand)
{
        ps_list_cursor item = {.at = list};
        bool any = false;
        positive before = address_to count;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                if (!item.length)
                        break;

                positive value;
                string_address at = item.from;

                if (!string_digits_checked(address_of at, 10, address_of value) ||
                    !value || (positive)(at - item.from) != item.length)
                        return false;

                /*
                        procps preserves a duplicate written in one -p list,
                        but repeated selection operands are unioned. Other
                        numeric selectors are sets in both shapes.
                */
                bool seen = ps_value_has(address_to values,
                                         duplicate_within_operand
                                             ? before
                                             : address_to count,
                                         value);

                if (!seen && !ps_value_add(values, count, room, value))
                        return false;

                any = true;
        }

        return any;
}

static bool ps_string_add(string_address address_to address_to values,
                          positive address_to count, positive address_to room,
                          string_address from, positive length)
{
        for (positive i = 0; i < address_to count; i++)
                if (string_length((address_to values)[i]) == length &&
                    !string_compare_max((address_to values)[i], from, length))
                        return true;

        if (!text_arena_grow(values, room, address_to count,
                             address_to count + 1,
                             sizeof(string_address), 16))
                return false;

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return false;

        memory_copy_apart(made, from, length);
        made[length] = end;
        (address_to values)[address_to count] = (string_address)made;
        address_to count += 1;
        return true;
}

static bool ps_command_list(string_address list,
                            string_address address_to address_to values,
                            positive address_to count, positive address_to room)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                if (!item.length)
                        break;

                if (!ps_string_add(values, count, room, item.from,
                                   item.length))
                        return false;

                any = true;
        }

        return any;
}

static bool ps_command_selected(string_address address_to values,
                                positive count, string_address command)
{
        positive command_length = string_length(command);

        for (positive i = 0; i < count; i++)
        {
                positive length = string_length(values[i]);

                /* procps compares -C through Linux's 15-byte comm ceiling. */
                if (length > 15)
                        length = 15;

                if (command_length == length &&
                    !string_compare_max(values[i], command, length))
                        return true;
        }

        return false;
}

static bool ps_sort_pid(string_address list, bool address_to reverse)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) ", "))
        {
                string_address from = item.from;
                positive length = item.length;

                if (!length)
                        return false;

                bool descending = false;

                if (string_get(from) == '+' || string_get(from) == '-')
                {
                        descending = string_get(from++) == '-';
                        length--;
                }

                if (length != 3 || string_compare_max(from,
                                                       (string_address)"pid", 3))
                        return false;

                if (!any)
                        address_to reverse = descending;

                any = true;
        }

        return any;
}

static positive ps_pid_matches(positive address_to values, positive count,
                               positive pid)
{
        positive matches = 0;

        for (positive i = 0; i < count; i++)
                if (values[i] == pid)
                        matches++;

        return matches;
}

static bool ps_format_list(string_address list,
                           ps_selected address_to address_to fields,
                           positive address_to count, positive address_to room)
{
        ps_list_cursor item = {.at = list};
        bool any = false;

        while (ps_list_next(address_of item, (string_address) "=, "))
        {
                string_address name_from = item.from;
                positive name_length = item.length;

                if (!name_length)
                {
                        if (!string_get(name_from))
                                break;

                        return false;
                }

                p8 address_to name =
                    (p8 address_to)text_arena_take(name_length + 1);

                if (!name)
                        return false;

                memory_copy_apart(name, name_from, name_length);
                name[name_length] = end;

                bool custom = string_get(item.at) == '=';
                string_address header = null;

                if (custom)
                {
                        string_address header_from = ++item.at;
                        positive header_length = string_span_without_set(
                            item.at, (string_address) ",");

                        item.at += header_length;
                        p8 address_to made =
                            (p8 address_to)text_arena_take(header_length + 1);

                        if (!made)
                                return false;

                        if (header_length)
                                memory_copy_apart(made, header_from, header_length);

                        made[header_length] = end;
                        header = (string_address)made;
                }

                positive which;
                string_address alias_header = null;

                if (string_equals(name, "cmd"))
                {
                        which = PS_FIELD_ARGS;
                        alias_header = "CMD";
                }
                else if (string_equals(name, "command"))
                {
                        which = PS_FIELD_ARGS;
                        alias_header = "COMMAND";
                }
                else if (string_equals(name, "ucmd"))
                {
                        which = PS_FIELD_COMM;
                        alias_header = "CMD";
                }
                else
                {
                        which = string_table_find(name, ps_columns,
                                                  sizeof(ps_columns[0]),
                                                  PS_FIELD_COUNT);
                }

                if (which == PS_FIELD_COUNT)
                {
                        text_error(name, "unknown user-defined format specifier");
                        return false;
                }

                if (!custom && alias_header)
                {
                        custom = true;
                        header = alias_header;
                }

                if (!ps_field_add(fields, count, room, which, header, custom))
                        return false;

                any = true;
        }

        return any;
}

/*
        The long selections arrive both as --name value and --name=value.
        One reader answers whether the word is this option, and hands back
        the value: null when the word that should have held it is missing,
        which each caller's validator already refuses.
*/
static bool ps_long_value(string_address argument, string_address name,
                          b32 address_to at, string_address address_to value)
{
        positive length = string_length(name);

        if (environment_key_is(argument, name, length))
        {
                address_to value = argument + length + 1;
                return true;
        }

        if (!string_equals(argument, name))
                return false;

        address_to value = program_argument(++(address_to at));
        return true;
}

static b32 tools_ps(void)
{
        ps_selected address_to fields = null;
        positive field_count = 0;
        positive field_room = 0;
        positive address_to selected_pids = null;
        positive selected_count = 0;
        positive selected_room = 0;
        positive address_to selected_ppids = null;
        positive ppid_count = 0;
        positive ppid_room = 0;
        string_address address_to selected_commands = null;
        positive command_count = 0;
        positive command_room = 0;
        bool every = false;
        bool full = false;
        bool no_headers = false;
        bool force_headers = false;
        bool reverse = false;

        text_begin("ps");
        text_arena_used = 0;
        ps_room = null;
        ps_room_used = 0;
        ps_room_size = 0;
        ps_failed = false;

        // Default formats change these three entries for display. Restore
        // the -o table for another invocation in the same process.
        ps_columns[PS_FIELD_TTY].header = "TT";
        ps_columns[PS_FIELD_TTY].width = 2;
        ps_columns[PS_FIELD_COMM].header = "COMMAND";
        ps_columns[PS_FIELD_ARGS].header = "COMMAND";

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = program_argument(i);
                string_address value;

                if (string_equals(argument, "--no-headers"))
                {
                        no_headers = true;
                        force_headers = false;
                        continue;
                }
                else if (string_equals(argument, "--headers"))
                {
                        force_headers = true;
                        no_headers = false;
                        continue;
                }
                else if (ps_long_value(argument, "--format", address_of i,
                                       address_of value))
                {
                        if (!value)
                        {
                                text_error(null, "option requires an argument -- format");
                                return text_done(1);
                        }

                        argument = value;
                }
                else if (ps_long_value(argument, "--pid", address_of i,
                                       address_of value))
                {
                        if (!value ||
                            !ps_pid_list(value, address_of selected_pids,
                                         address_of selected_count,
                                         address_of selected_room, true))
                        {
                                text_error(value, "invalid process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (ps_long_value(argument, "--ppid", address_of i,
                                       address_of value))
                {
                        if (!value ||
                            !ps_pid_list(value, address_of selected_ppids,
                                         address_of ppid_count,
                                         address_of ppid_room, false))
                        {
                                text_error(value, "invalid parent process id list");
                                return text_done(1);
                        }

                        continue;
                }
                else if (ps_long_value(argument, "--sort", address_of i,
                                       address_of value))
                {
                        if (!value || !ps_sort_pid(value, address_of reverse))
                        {
                                text_error(value, "unsupported sort key");
                                return text_done(1);
                        }

                        continue;
                }
                else
                {
                        /*
                                A word of letters, with or without the dash.
                                o, p and C carry a value, the rest of the
                                word when there is one and the next argument
                                otherwise, so any of them ends the word: -eo
                                pid and -fp 1 are -e -o pid and -f -p 1,
                                which is how procps reads them.
                        */
                        string_address at = argument;
                        bool dashed = string_get(at) == '-';
                        p8 taking = 0;

                        if (dashed)
                                at++;

                        bool known = string_get(at) != end;

                        for (; string_get(at) && !taking; at++)
                                switch (string_get(at))
                                {
                                case 'e':
                                case 'A': every = true; break;
                                case 'f': full = true; break;
                                case 'w': break;
                                case 'h': no_headers = true; break;
                                case 'o':
                                case 'p':
                                case 'C':
                                        // BSD C is a CPU accounting switch,
                                        // not the selector -C is.
                                        if (string_get(at) == 'C' && !dashed)
                                        {
                                                known = false;
                                                break;
                                        }

                                        taking = string_get(at);
                                        value = string_get(at + 1)
                                                    ? at + 1
                                                    : program_argument(++i);
                                        break;
                                case 'a':
                                case 'x':
                                case 'u':
                                        /*
                                                These BSD personalities also
                                                replace the output format.
                                                A broad default listing is a
                                                plausible but wrong `ps aux`,
                                                so refuse them until that
                                                format exists in full.
                                        */
                                        known = false;
                                        break;
                                default: known = false; break;
                                }

                        if (!known)
                        {
                                text_error(argument, "unsupported option");
                                return text_done(1);
                        }

                        if (taking == 'p')
                        {
                                if (!value ||
                                    !ps_pid_list(value, address_of selected_pids,
                                                 address_of selected_count,
                                                 address_of selected_room,
                                                 true))
                                {
                                        text_error(value,
                                                   "invalid process id list");
                                        return text_done(1);
                                }

                                continue;
                        }

                        if (taking == 'C')
                        {
                                if (!value ||
                                    !ps_command_list(value,
                                                     address_of selected_commands,
                                                     address_of command_count,
                                                     address_of command_room))
                                {
                                        text_error(value, "invalid command list");
                                        return text_done(1);
                                }

                                continue;
                        }

                        if (!taking)
                                continue;

                        if (!value)
                        {
                                text_error(null, "option requires an argument -- o");
                                return text_done(1);
                        }

                        argument = value;
                }

                if (!ps_format_list(argument, address_of fields,
                                    address_of field_count,
                                    address_of field_room))
                        return text_done(1);
        }

        /*
                The two listings ps has of its own are not -o spelled out:
                the terminal is eight columns wide and headed TTY rather than
                two and TT, and the command is headed CMD. The table above is
                what -o asks for, so the two that differ are set here.
        */
        if (!field_count)
        {
                // The two listings ps prints with no -o, in the shape -o
                // carries: field, header, and whether the header overrides
                // the column table's.
                static const ps_selected ps_full_preset[] = {
                    {PS_FIELD_USER, "UID", true},  {PS_FIELD_PID, null, false},
                    {PS_FIELD_PPID, null, false},  {PS_FIELD_CPU, null, false},
                    {PS_FIELD_STIME, null, false}, {PS_FIELD_TTY, null, false},
                    {PS_FIELD_TIME, null, false},  {PS_FIELD_ARGS, "CMD", true},
                };
                static const ps_selected ps_plain_preset[] = {
                    {PS_FIELD_PID, null, false},   {PS_FIELD_TTY, null, false},
                    {PS_FIELD_TIME, null, false},  {PS_FIELD_COMM, "CMD", true},
                };
                const ps_selected address_to preset = full ? ps_full_preset
                                                           : ps_plain_preset;
                positive presets = full ? 8 : 4;

                ps_columns[PS_FIELD_TTY].header = "TTY";
                ps_columns[PS_FIELD_TTY].width = 8;
                ps_columns[full ? PS_FIELD_ARGS : PS_FIELD_COMM].header = "CMD";

                for (positive f = 0; f < presets; f++)
                        if (!ps_field_add(address_of fields,
                                          address_of field_count,
                                          address_of field_room,
                                          preset[f].field, preset[f].header,
                                          preset[f].custom_header))
                                return text_done(1);
        }

        positive wanted = 0;

        for (positive f = 0; f < field_count; f++)
                wanted |= (positive)1 << fields[f].field;

        bool selectors = selected_count || ppid_count || command_count;
        bool alternate_selectors = ppid_count || command_count;
        bool filter_owner = !every && !selectors;
        bool names = wanted & ((positive)1 << PS_FIELD_USER);
        bool owners = filter_owner || names ||
                      (wanted & ((positive)1 << PS_FIELD_UID));
        if (!system_snapshot_take(address_of ps_snapshot,
                                  SPARK_SNAPSHOT_PROCESS, owners))
        {
                text_error("/proc", "cannot read");
                return text_done(1);
        }

        ps_now = ps_snapshot.header.uptime_ns / SYSTEM_NANOSECONDS;
        ps_wall = ps_snapshot.header.realtime_seconds;
        positive ps_count = ps_snapshot.header.process_count;

        if (filter_owner)
        {
                ps_own_tty = 0;
                positive own_pid = (positive)system_call(syscall(getpid));

                for (positive i = 0; i < ps_count; i++)
                        if (ps_snapshot.processes[i].pid == own_pid)
                        {
                                ps_own_tty =
                                    (positive)ps_snapshot.processes[i].tty;
                                break;
                        }

                ps_own_uid = (positive)system_call(syscall(geteuid));
        }

        bool show_headers = force_headers;

        if (!force_headers && !no_headers)
                for (positive f = 0; f < field_count; f++)
                {
                        string_address header;

                        ps_column_width(fields + f, address_of header);

                        if (header && string_get(header))
                        {
                                show_headers = true;
                                break;
                        }
                }

        if (show_headers)
        {
                for (positive f = 0; f < field_count; f++)
                {
                        positive field = fields[f].field;
                        bool last = f + 1 == field_count;
                        string_address header;
                        positive width = ps_column_width(fields + f,
                                                         address_of header);

                        string_to_field(text_put,
                                        header ? header : (string_address)"",
                                        !ps_columns[field].right && last ? 0 : width,
                                        ' ', !ps_columns[field].right);

                        if (!last)
                                text_put_character(' ');
                }

                text_put_character('\n');
        }

        bool matched = false;

        for (positive at = 0; at < ps_count; at++)
        {
                positive p = reverse ? ps_count - at - 1 : at;
                struct snapshot_process address_to process =
                    ps_snapshot.processes + p;
                positive repeats = 1;

                /*
                        procps gives -p its historical override of -e, but
                        combines -e with -C/--ppid as a union, which is all
                        processes. Preserve that awkward visible distinction.
                */
                if (selectors && !(every && alternate_selectors))
                {
                        repeats = ps_pid_matches(selected_pids, selected_count,
                                                 process->pid);

                        if (!repeats &&
                            (ps_value_has(selected_ppids, ppid_count,
                                          process->ppid) ||
                             ps_command_selected(selected_commands,
                                                 command_count,
                                                 process->command)))
                                repeats = 1;

                        if (!repeats)
                                continue;
                }
                else if (!every &&
                         !(process->uid == ps_own_uid &&
                           (positive)process->tty == ps_own_tty))
                        continue;

                matched = true;
                ps_detail detail = {0};

                for (positive repeat = 0; repeat < repeats; repeat++)
                {
                        for (positive f = 0; f < field_count; f++)
                                ps_column_out(process, address_of detail,
                                              fields[f].field,
                                              ps_column_width(fields + f, null),
                                              f + 1 == field_count);

                        text_put_character('\n');
                }
        }

        return text_done(ps_failed || (selectors && !matched) ? 1 : 0);
}

// dmesg -----------------------------------------------------

/* Linux's syslog(2) operation numbers are an ABI, despite the syscall's
   unfortunate collision with libc's unrelated syslog function. */
#define DMESG_CLOSE 0
#define DMESG_OPEN 1
#define DMESG_READ 2
#define DMESG_READ_ALL 3
#define DMESG_READ_CLEAR 4
#define DMESG_CLEAR 5
#define DMESG_CONSOLE_OFF 6
#define DMESG_CONSOLE_ON 7
#define DMESG_CONSOLE_LEVEL 8
#define DMESG_SIZE_UNREAD 9
#define DMESG_SIZE_BUFFER 10

typedef struct
{
        p8 address_to raw;
        positive raw_length;
        p8 address_to message;
        positive message_length;
        p64 microseconds;
        p32 priority;
        bool priority_known;
        bool time_known;
        bool timestamp_present;
        bool continuation;
} tools_dmesg_record;

typedef struct
{
        p32 levels;
        p32 facilities;
        p64 previous;
        p64 realtime_base;
        positive rows;
        bool raw;
        bool decode;
        bool no_time;
        bool ctime;
        bool relative;
        bool delta;
        bool json;
        bool noescape;
        bool color;
        bool previous_known;
} tools_dmesg_state;

static const string_address tools_dmesg_levels[] = {
    (string_address)"emerg", (string_address)"alert",
    (string_address)"crit", (string_address)"err",
    (string_address)"warn", (string_address)"notice",
    (string_address)"info", (string_address)"debug",
};

static const string_address tools_dmesg_facilities[] = {
    (string_address)"kern", (string_address)"user",
    (string_address)"mail", (string_address)"daemon",
    (string_address)"auth", (string_address)"syslog",
    (string_address)"lpr", (string_address)"news",
    (string_address)"uucp", (string_address)"cron",
    (string_address)"authpriv", (string_address)"ftp",
    (string_address)"res0", (string_address)"res1",
    (string_address)"res2", (string_address)"res3",
    (string_address)"local0", (string_address)"local1",
    (string_address)"local2", (string_address)"local3",
    (string_address)"local4", (string_address)"local5",
    (string_address)"local6", (string_address)"local7",
};

static const file_long tools_dmesg_longs[] = {
    {(string_address)"clear", 'C'},
    {(string_address)"read-clear", 'c'},
    {(string_address)"console-off", 'D'},
    {(string_address)"console-on", 'E'},
    {(string_address)"file", 'F'},
    {(string_address)"kmsg-file", 'K'},
    {(string_address)"facility", 'f'},
    {(string_address)"human", 'H'},
    {(string_address)"json", 'J'},
    {(string_address)"kernel", 'k'},
    {(string_address)"color", 'L'},
    {(string_address)"level", 'l'},
    {(string_address)"console-level", 'n'},
    {(string_address)"nopager", 'P'},
    {(string_address)"force-prefix", 'p'},
    {(string_address)"raw", 'r'},
    {(string_address)"noescape", 'N'},
    {(string_address)"syslog", 'S'},
    {(string_address)"buffer-size", 's'},
    {(string_address)"userspace", 'u'},
    {(string_address)"follow", 'w'},
    {(string_address)"follow-new", 'W'},
    {(string_address)"decode", 'x'},
    {(string_address)"show-delta", 'd'},
    {(string_address)"reltime", 'e'},
    {(string_address)"ctime", 'T'},
    {(string_address)"notime", 't'},
    {(string_address)"time-format", 'q'},
    {(string_address)"since", 'a'},
    {(string_address)"until", 'b'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static bool tools_dmesg_span_number(p8 address_to address_to cursor,
                                    p8 address_to stop, p8 delimiter,
                                    positive address_to value)
{
        p8 address_to at = address_to cursor;
        positive made = 0;
        bool any = false;

        while (at < stop && byte_is_digit(*at))
        {
                positive digit = *at++ - '0';

                if (made > (positive_max - digit) / 10)
                        return false;
                made = made * 10 + digit;
                any = true;
        }
        if (!any || at == stop || *at != delimiter)
                return false;
        address_to cursor = at + 1;
        address_to value = made;
        return true;
}

static bool tools_dmesg_legacy_record(p8 address_to bytes, positive length,
                                      p32 inherited,
                                      tools_dmesg_record address_to record)
{
        memory_fill(record, 0, sizeof(*record));
        record->raw = bytes;
        record->raw_length = length;
        record->priority = inherited;

        p8 address_to at = bytes;
        p8 address_to stop = bytes + length;
        if (at < stop && *at == '<')
        {
                positive priority;

                at++;
                if (!tools_dmesg_span_number(address_of at, stop, '>',
                                              address_of priority) ||
                    priority > 191)
                        return false;
                record->priority = (p32)priority;
                record->priority_known = true;
        }
        else
                record->continuation = true;

        p8 address_to stamp = at;
        if (at < stop && *at == '[')
        {
                at++;
                while (at < stop && *at == ' ')
                        at++;
                positive seconds = 0;
                bool any = false;
                while (at < stop && byte_is_digit(*at))
                {
                        positive digit = *at++ - '0';
                        if (seconds > (positive_max - digit) / 10)
                                return false;
                        seconds = seconds * 10 + digit;
                        any = true;
                }

                positive fraction = 0;
                positive digits = 0;
                if (at < stop && *at == '.')
                {
                        at++;
                        while (at < stop && byte_is_digit(*at))
                        {
                                if (digits < 6)
                                        fraction = fraction * 10 + (*at - '0');
                                digits++;
                                at++;
                        }
                }
                if (any && at < stop && *at == ']')
                {
                        record->microseconds = (p64)seconds * 1000000 +
                                               fraction;
                        record->time_known = true;
                        record->timestamp_present = true;
                        at++;
                        if (at < stop && *at == ' ')
                                at++;
                }
                else
                        at = stamp;
        }

        /* A priority with no timestamp is still a complete syslog record;
           util-linux renders its missing kernel clock as zero. */
        if (record->priority_known && !record->time_known)
                record->time_known = true;

        record->message = at;
        record->message_length = (positive)(stop - at);
        return true;
}

static bool tools_dmesg_kmsg_record(p8 address_to bytes, positive length,
                                    tools_dmesg_record address_to record)
{
        memory_fill(record, 0, sizeof(*record));
        record->raw = bytes;
        record->raw_length = length;

        p8 address_to at = bytes;
        p8 address_to stop = bytes + length;
        positive priority;
        positive sequence;
        positive microseconds;

        if (!tools_dmesg_span_number(address_of at, stop, ',',
                                      address_of priority) ||
            !tools_dmesg_span_number(address_of at, stop, ',',
                                      address_of sequence) ||
            !tools_dmesg_span_number(address_of at, stop, ',',
                                      address_of microseconds) ||
            priority > 191)
                return false;
        (void)sequence;

        while (at < stop && *at != ';')
                at++;
        if (at == stop)
                return false;
        at++;

        p8 address_to message_stop = at;
        while (message_stop < stop && *message_stop != '\n' && *message_stop)
                message_stop++;

        record->priority = (p32)priority;
        record->priority_known = true;
        record->microseconds = microseconds;
        record->time_known = true;
        record->timestamp_present = true;
        record->message = at;
        record->message_length = (positive)(message_stop - at);
        return true;
}

static b32 tools_dmesg_named(p8 address_to word, positive length,
                             const string_address address_to names,
                             positive count)
{
        for (positive i = 0; i < count; i++)
                if (file_same_word(word, length, names[i]))
                        return (b32)i;

        if (names == tools_dmesg_levels)
        {
                if (file_same_word(word, length, (string_address)"panic"))
                        return 0;
                if (file_same_word(word, length, (string_address)"error"))
                        return 3;
                if (file_same_word(word, length, (string_address)"warning"))
                        return 4;
        }
        return -1;
}

static bool tools_dmesg_mask(string_address list,
                             const string_address address_to names,
                             positive name_count, p32 address_to result,
                             bool ranges)
{
        ps_list_cursor item = {.at = list};
        p32 mask = 0;

        if (!string_get(list))
        {
                address_to result = ((p32)1 << name_count) - 1;
                return true;
        }

        positive first_nonblank = string_span(list, string_set_blanks);
        if (string_is(list + first_nonblank, ','))
                return false;
        for (positive at = first_nonblank; string_get(list + at); at++)
                if (string_is(list + at, ','))
                {
                        positive next = at + 1;
                        while (byte_is_space(string_get(list + next)))
                                next++;
                        if (!string_get(list + next) ||
                            string_is(list + next, ','))
                                return false;
                }

        while (ps_list_next(address_of item, (string_address)", "))
        {
                if (!item.length)
                        return false;

                bool from_zero = ranges && item.from[item.length - 1] == '+';
                bool through_end = ranges && item.from[0] == '+';
                p8 address_to word = item.from + through_end;
                positive length = item.length - from_zero - through_end;
                b32 number = tools_dmesg_named(word, length, names,
                                                name_count);

                if (number < 0 && length == 1 && byte_is_digit(*word) &&
                    (positive)(*word - '0') < name_count)
                        number = *word - '0';
                if (number < 0 || !length || (from_zero && through_end))
                        return false;

                positive first = from_zero ? 0 : (positive)number;
                positive last = through_end ? name_count - 1
                                            : (positive)number;
                for (positive bit = first; bit <= last; bit++)
                        mask |= (p32)1 << bit;
        }

        address_to result = mask;
        return mask != 0;
}

static fn tools_dmesg_timestamp(p64 microseconds, bool signed_value,
                                bool negative)
{
        positive seconds = (positive)(microseconds / 1000000);
        positive fraction = (positive)(microseconds % 1000000);

        text_put_character('[');
        if (signed_value)
        {
                p8 number[24];
                positive digits = positive_into(number, seconds);
                writer_fill(text_put, digits < 3 ? 3 - digits : 0, ' ');
                text_put_character(negative ? '-' : '+');
                text_put(number, digits);
        }
        else
                positive_to_padded(text_put, seconds, 5, ' ', 0);
        text_put_character('.');
        positive_to_padded(text_put, fraction, 6, '0', 0);
        text_put_string("] ");
}

static fn tools_dmesg_timestamp_delta(p64 microseconds, p64 delta,
                                      bool negative)
{
        text_put_character('[');
        positive_to_padded(text_put, (positive)(microseconds / 1000000),
                           5, ' ', 0);
        text_put_character('.');
        positive_to_padded(text_put, (positive)(microseconds % 1000000),
                           6, '0', 0);
        text_put_string(" <");
        if (negative)
        {
                p8 number[24];
                positive seconds = (positive)(delta / 1000000);
                positive digits = positive_into(number, seconds);
                writer_fill(text_put, digits < 4 ? 4 - digits : 0, ' ');
                text_put_character('-');
                text_put(number, digits);
        }
        else
                positive_to_padded(text_put, (positive)(delta / 1000000),
                                   5, ' ', 0);
        text_put_character('.');
        positive_to_padded(text_put, (positive)(delta % 1000000),
                           6, '0', 0);
        text_put_string(">] ");
}

static fn tools_dmesg_calendar(tools_dmesg_state address_to state,
                               p64 microseconds, bool compact)
{
        time_t stamp = (time_t)(state->realtime_base +
                                microseconds / 1000000);
        tm broken;
        p8 made[64];
        string_address format = compact ? (string_address)"%b%e %H:%M"
                                        : (string_address)"%a %b %e %H:%M:%S %Y";

        if (!localtime_r(address_of stamp, address_of broken))
                return;
        positive length = clock_format_extended(made, sizeof(made), format,
                                                address_of broken);
        if (!length)
                return;
        text_put_character('[');
        text_put(made, length);
        text_put_string("] ");
}

static fn tools_dmesg_message(p8 address_to bytes, positive length,
                              bool noescape)
{
        for (positive i = 0; i < length; i++)
        {
                p8 byte = bytes[i];

                if (noescape || byte == '\t' || (byte >= ' ' && byte != 0x7f))
                        text_put_character(byte);
                else
                {
                        p8 escaped[4] = {'\\', 'x',
                            storage_hex_digit(byte >> 4, false),
                            storage_hex_digit(byte & 15, false)};
                        text_put(escaped, sizeof(escaped));
                }
        }
}

static fn tools_dmesg_emit(tools_dmesg_state address_to state,
                           tools_dmesg_record address_to record)
{
        positive level = record->priority & 7;
        positive facility = record->priority >> 3;

        if (facility >= array_count(tools_dmesg_facilities) ||
            !(state->levels & ((p32)1 << level)) ||
            !(state->facilities & ((p32)1 << facility)))
                return;

        if (state->json)
        {
                text_put_string(state->rows ? ",{\n" : "      {\n");
                text_put_string("         \"pri\": ");
                positive_to_string(text_put, record->priority);
                text_put_string(",\n         \"time\": ");
                positive_to_padded(text_put,
                                   (positive)(record->microseconds / 1000000),
                                   5, ' ', 0);
                text_put_character('.');
                positive_to_padded(text_put,
                                   (positive)(record->microseconds % 1000000),
                                   6, '0', 0);
                text_put_string(",\n         \"msg\": ");
                column_json_string((column_cell){record->message,
                                                  record->message_length},
                                   false);
                text_put_string("\n      }");
                state->rows++;
                return;
        }

        if (state->raw)
        {
                if (record->priority_known)
                {
                        text_put_character('<');
                        positive_to_string(text_put, record->priority);
                        text_put_character('>');
                }
                if (record->timestamp_present)
                        tools_dmesg_timestamp(record->microseconds, false,
                                              false);
                tools_dmesg_message(record->message, record->message_length,
                                    state->noescape);
                text_put_character('\n');
                state->rows++;
                return;
        }

        if (state->decode)
        {
                if (record->continuation)
                        writer_fill(text_put, 15, ' ');
                else
                {
                        string_address name = tools_dmesg_facilities[facility];
                        text_put_string(name);
                        writer_fill(text_put, 6 - string_length(name), ' ');
                        text_put_character(':');
                        name = tools_dmesg_levels[level];
                        text_put_string(name);
                        writer_fill(text_put, 6 - string_length(name), ' ');
                        text_put_string(": ");
                }
        }

        p64 delta = 0;
        bool delta_negative = false;
        if (state->previous_known)
        {
                delta_negative = record->microseconds < state->previous;
                delta = delta_negative ? state->previous - record->microseconds
                                       : record->microseconds - state->previous;
        }

        if (!state->no_time)
        {
                if (record->continuation)
                        writer_fill(text_put, 15, ' ');
                else
                {
                        if (state->relative)
                        {
                                if (!state->previous_known)
                                        tools_dmesg_calendar(state,
                                                             record->microseconds,
                                                             true);
                                else
                                        tools_dmesg_timestamp(delta, true,
                                                              delta_negative);
                        }
                        if (state->ctime)
                                tools_dmesg_calendar(state,
                                                     record->microseconds,
                                                     false);
                        if (!state->relative && !state->ctime)
                        {
                                if (state->color)
                                        text_put_string("\033[32m");
                                if (state->delta)
                                        tools_dmesg_timestamp_delta(
                                            record->microseconds, delta,
                                            delta_negative);
                                else
                                        tools_dmesg_timestamp(
                                            record->microseconds, false,
                                            false);
                                if (state->color)
                                        text_put_string("\033[0m");
                        }
                }
        }

        if (state->color && level <= 3)
                text_put_string("\033[31m");
        tools_dmesg_message(record->message, record->message_length,
                            state->noescape);
        if (state->color && level <= 3)
                text_put_string("\033[0m");
        text_put_character('\n');
        if (record->time_known)
        {
                state->previous = record->microseconds;
                state->previous_known = true;
        }
        state->rows++;
}

static fn tools_dmesg_buffer(tools_dmesg_state address_to state,
                             p8 address_to bytes, positive length, bool kmsg)
{
        positive at = 0;
        p32 inherited = 7;

        while (at < length)
        {
                positive stop = at;
                p8 delimiter = kmsg ? '\0' : '\n';

                while (stop < length && bytes[stop] != delimiter)
                        stop++;
                positive record_length = stop - at;
                if (kmsg && record_length && bytes[stop - 1] == '\n')
                        record_length--;

                tools_dmesg_record record;
                bool valid = kmsg
                    ? tools_dmesg_kmsg_record(bytes + at, record_length,
                                               address_of record)
                    : tools_dmesg_legacy_record(bytes + at, record_length,
                                                inherited,
                                                address_of record);
                if (valid)
                {
                        inherited = record.priority;
                        tools_dmesg_emit(state, address_of record);
                }

                at = stop < length ? stop + 1 : length;
        }
}

static b32 tools_dmesg_read_file(tools_dmesg_state address_to state,
                                 string_address path, bool kmsg)
{
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        if (handle < 0)
        {
                text_flush();
                string_format(file_fail, "dmesg: cannot open %s: %s\n",
                              path, file_reason(handle));
                return text_done(1);
        }

        positive length = 0;
        bool failed = false;
        p8 address_to bytes = text_arena_read_all(
            (positive)handle, 16384, address_of length, address_of failed);
        system_close(handle);
        if (!bytes)
                return text_done(1);

        if (state->json)
                text_put_string("{\n   \"dmesg\": [\n");
        tools_dmesg_buffer(state, bytes, length, kmsg);
        if (state->json)
                text_put_string(state->rows ? "\n   ]\n}\n"
                                           : "   ]\n}\n");
        return text_done(failed ? 1 : 0);
}

static b32 tools_dmesg_follow(tools_dmesg_state address_to state,
                              bool only_new)
{
#if defined(LINUX)
        bipolar handle = system_open_at(AT_FDCWD, "/dev/kmsg",
                                        FILE_READ | O_CLOEXEC);
        if (handle < 0)
                return text_refuse("read kernel buffer failed",
                                   file_reason(handle), 1);
        if (only_new && system_seek(handle, 0, FILE_SEEK_END) < 0)
        {
                system_close(handle);
                return text_refuse("read kernel buffer failed",
                                   "cannot seek /dev/kmsg", 1);
        }

        p8 record_bytes[65536];
        while (!text_out_failed)
        {
                bipolar got = system_read_retry((positive)handle,
                                                record_bytes,
                                                sizeof(record_bytes));
                if (got < 0)
                {
                        system_close(handle);
                        return text_refuse("read kernel buffer failed",
                                           file_reason(got), 1);
                }
                if (!got)
                        continue;
                tools_dmesg_record record;
                if (tools_dmesg_kmsg_record(record_bytes, (positive)got,
                                             address_of record))
                        tools_dmesg_emit(state, address_of record);
                text_flush();
        }
        system_close(handle);
        return text_done(1);
#else
        (void)state;
        (void)only_new;
        return text_refuse(null, "kernel log requires Linux", 1);
#endif
}

static b32 tools_dmesg_control(b32 operation, positive value,
                               string_address action)
{
#if defined(LINUX)
        bipolar answer = system_call_3(syscall(syslog), operation, 0, value);
        return answer < 0 ? text_refuse(action, file_reason(answer), 1)
                          : text_done(0);
#else
        (void)operation;
        (void)value;
        return text_refuse(action, "requires Linux", 1);
#endif
}

static b32 tools_dmesg_main()
{
        file_taking taking = {
            .program = (string_address)"dmesg",
            .allowed = (string_address)"CcDEFKfHjkLlnoPprSsuwWxdeTtJVh",
            .valued = (string_address)"FKflnsqab",
            .long_optional = (string_address)"L",
            .longs = tools_dmesg_longs,
        };

        text_begin("dmesg");
        text_arena_used = 0;
        if (!file_take(address_of taking))
                return text_done(1);
        if (taking.first != (positive)program_argument_count())
                return text_refuse(program_argument((b32)taking.first),
                                   "extra operand", 1);
        if (taking.flags & FILE_FLAG('h'))
        {
                text_put_string("Usage: dmesg [options]\n"
                                "Display or control the kernel ring buffer.\n");
                return text_done(0);
        }
        if (taking.flags & FILE_FLAG('V'))
        {
                text_put_string("dmesg from dawning-kit\n");
                return text_done(0);
        }

        positive flags = taking.flags;
        if ((flags & FILE_FLAG('F')) && (flags & FILE_FLAG('K')))
                return text_refuse(null, "--file and --kmsg-file cannot be combined", 1);
        if ((flags & FILE_FLAG('r')) &&
            (flags & (FILE_FLAG('x') | FILE_FLAG('t') | FILE_FLAG('T') |
                      FILE_FLAG('e') | FILE_FLAG('d') | FILE_FLAG('J'))))
                return text_refuse(null, "--raw cannot be combined with decoded output", 1);
        if ((flags & FILE_FLAG('J')) &&
            (flags & (FILE_FLAG('T') | FILE_FLAG('e') | FILE_FLAG('d'))))
                return text_refuse(null, "JSON time transformations are not supported", 1);
        if (flags & (FILE_FLAG('a') | FILE_FLAG('b')))
                return text_refuse(null, "--since and --until are not supported", 1);

        positive control_count = ((flags & FILE_FLAG('C')) != 0) +
                                 ((flags & FILE_FLAG('D')) != 0) +
                                 ((flags & FILE_FLAG('E')) != 0) +
                                 ((flags & FILE_FLAG('n')) != 0);
        if (control_count > 1)
                return text_refuse(null, "kernel log controls cannot be combined", 1);

        if (flags & FILE_FLAG('C'))
                return tools_dmesg_control(DMESG_CLEAR, 0,
                                           "clear kernel buffer failed");
        if (flags & FILE_FLAG('D'))
                return tools_dmesg_control(DMESG_CONSOLE_OFF, 0,
                                           "klogctl failed");
        if (flags & FILE_FLAG('E'))
                return tools_dmesg_control(DMESG_CONSOLE_ON, 0,
                                           "klogctl failed");
        if (flags & FILE_FLAG('n'))
        {
                string_address level_name = file_option_value(address_of taking,
                                                               'n');
                positive length = string_length(level_name);
                b32 level = tools_dmesg_named((p8 address_to)level_name, length,
                                              tools_dmesg_levels,
                                              array_count(tools_dmesg_levels));
                if (level < 0 && length == 1 && level_name[0] >= '1' &&
                    level_name[0] <= '8')
                        level = level_name[0] - '1';
                if (level < 0)
                        return text_refuse(level_name, "unknown console level", 1);
                return tools_dmesg_control(DMESG_CONSOLE_LEVEL,
                                           (positive)level + 1,
                                           "klogctl failed");
        }

        tools_dmesg_state state = {
            .levels = 0xff,
            .facilities = 0xffffff,
            .raw = (flags & FILE_FLAG('r')) != 0,
            .decode = (flags & FILE_FLAG('x')) != 0,
            .no_time = (flags & FILE_FLAG('t')) != 0,
            .ctime = (flags & FILE_FLAG('T')) != 0,
            .relative = (flags & (FILE_FLAG('e') | FILE_FLAG('H'))) != 0,
            .delta = (flags & FILE_FLAG('d')) != 0,
            .json = (flags & FILE_FLAG('J')) != 0,
            .noescape = (flags & FILE_FLAG('N')) != 0,
        };

        string_address value = file_option_value(address_of taking, 'q');
        if (value)
        {
                if (string_equals(value, "notime"))
                        state.no_time = true;
                else if (string_equals(value, "ctime"))
                        state.ctime = true;
                else if (string_equals(value, "reltime"))
                        state.relative = true;
                else if (string_equals(value, "delta"))
                        state.delta = true;
                else if (!string_equals(value, "raw"))
                        return text_refuse(value, "unsupported time format", 1);
        }

        value = file_option_value(address_of taking, 'l');
        if (value && !tools_dmesg_mask(value, tools_dmesg_levels,
                                       array_count(tools_dmesg_levels),
                                       address_of state.levels, true))
                return text_refuse(value, "unknown log level", 1);

        value = file_option_value(address_of taking, 'f');
        if (value && !tools_dmesg_mask(value, tools_dmesg_facilities,
                                       array_count(tools_dmesg_facilities),
                                       address_of state.facilities, false))
                return text_refuse(value, "unknown log facility", 1);
        if (flags & (FILE_FLAG('k') | FILE_FLAG('u')))
        {
                state.facilities = 0;
                if (flags & FILE_FLAG('k'))
                        state.facilities |= 1;
                if (flags & FILE_FLAG('u'))
                        state.facilities |= 0xfffffe;
        }

        b32 color = file_color_when(file_option_value(address_of taking, 'L'),
                                    FILE_COLOR_AUTO);
        if (color < 0)
                return text_refuse(file_option_value(address_of taking, 'L'),
                                   "invalid color mode", 1);
        state.color = file_color_active(color);

        p64 real = system_clock_ns(0);
        p64 boot = system_clock_ns(7);
        state.realtime_base = real >= boot
            ? (real - boot) / SYSTEM_NANOSECONDS : 0;

        positive capacity = 0;
        value = file_option_value(address_of taking, 's');
        if (value && (!text_unsigned_option(value, false,
                                             address_of capacity) ||
                      !capacity || capacity >= TEXT_ARENA_BYTES))
                return text_refuse(value, "invalid buffer size", 1);

        string_address file = file_option_value(address_of taking, 'F');
        string_address kmsg_file = file_option_value(address_of taking, 'K');
        if (file || kmsg_file)
        {
                if (flags & (FILE_FLAG('c') | FILE_FLAG('w') |
                             FILE_FLAG('W') | FILE_FLAG('p')))
                        return text_refuse(null, "selected mode requires a live kernel log", 1);
                return tools_dmesg_read_file(address_of state,
                                             file ? file : kmsg_file,
                                             kmsg_file != null);
        }

        if (flags & (FILE_FLAG('w') | FILE_FLAG('W')))
        {
                if (flags & FILE_FLAG('c'))
                        return text_refuse(null, "--read-clear and --follow cannot be combined", 1);
                if (state.json)
                        return text_refuse(null, "JSON follow output is not supported", 1);
                return tools_dmesg_follow(address_of state,
                                          (flags & FILE_FLAG('W')) != 0);
        }

#if defined(LINUX)
        if (!capacity)
        {
                bipolar size = system_call_3(syscall(syslog),
                                             DMESG_SIZE_BUFFER, 0, 0);
                capacity = size > 0 ? (positive)size : 16384;
        }
        p8 address_to buffer = (p8 address_to)text_arena_take(capacity + 1);
        if (!buffer)
                return text_done(1);
        b32 action = (flags & FILE_FLAG('c')) ? DMESG_READ_CLEAR
                                              : DMESG_READ_ALL;
        bipolar got = system_call_3(syscall(syslog), action,
                                    (positive)buffer, capacity);
        if (got < 0)
                return text_refuse("read kernel buffer failed",
                                   file_reason(got), 1);
        if (state.json)
                text_put_string("{\n   \"dmesg\": [\n");
        tools_dmesg_buffer(address_of state, buffer, (positive)got, false);
        if (state.json)
                text_put_string(state.rows ? "\n   ]\n}\n"
                                           : "   ]\n}\n");
        return text_done(0);
#else
        return text_refuse(null, "kernel log requires Linux", 1);
#endif
}

#include "util_linux.c"

// fincore ----------------------------------------------------------

/*
        cachestat is the constant-work answer on current kernels.  One whole
        file query gives the same cached-page count without walking a sparse
        mapping.  Kernels and policies that refuse that newer syscall fall
        through to mmap plus mincore.  The unclaimed tail of the shared text
        arena is scratch for the one-byte-per-page vector; file_transfer is
        the bounded fallback.  There is still one row engine and no pread.
*/
enum
{
        TOOLS_FINCORE_RES,
        TOOLS_FINCORE_PAGES,
        TOOLS_FINCORE_SIZE,
        TOOLS_FINCORE_FILE,
        TOOLS_FINCORE_COLUMNS,
};

typedef struct
{
        string_address path;
        p64 resident;
        p64 pages;
        p64 size;
} tools_fincore_row;

typedef struct
{
        p64 offset;
        p64 length;
} tools_fincore_range;

typedef struct
{
        p64 cached;
        p64 dirty;
        p64 writeback;
        p64 evicted;
        p64 recently_evicted;
} tools_fincore_cache;

static bool tools_fincore_bytes;

static ul_table_column tools_fincore_columns[] = {
    {(string_address)"res", (string_address)"RES", 0, true, UL_TABLE_STRING},
    {(string_address)"pages", (string_address)"PAGES", 0, true, UL_TABLE_NUMBER},
    {(string_address)"size", (string_address)"SIZE", 0, true, UL_TABLE_STRING},
    {(string_address)"file", (string_address)"FILE", 0, false, UL_TABLE_STRING},
};

static const file_long tools_fincore_longs[] = {
    {(string_address)"json", 'J'},
    {(string_address)"bytes", 'b'},
    {(string_address)"total", 'c'},
    {(string_address)"noheadings", 'n'},
    {(string_address)"output", 'o'},
    {(string_address)"output-all", 'A'},
    {(string_address)"raw", 'r'},
    {(string_address)"recursive", 'R'},
    {(string_address)"cachestat", 'C'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

/* Adapt the shared nearest IEC formatter to util-linux's compact spelling:
   "1.5 KiB" becomes "1.5K", "8.0 KiB" becomes "8K", and "3 B"
   becomes "3B".  Keep the unit selected by the value, since fincore prints
   1048575 as 1024K instead of carrying the rounded value into 1M. */
static fn tools_fincore_human(p8 address_to into, p64 value)
{
        positive unit = 0;
        p64 scaled = value;
        while (scaled >= 1024 && unit < 6)
        {
                scaled >>= 10;
                unit++;
        }

        p8 made[24];
        positive length = positive_into_human_nearest_string(
            made, (positive)value, true);
        positive space = 0;
        while (space < length && made[space] != ' ')
                space++;

        static const p8 units[] = "BKMGTPE";
        p8 actual = space + 1 < length ? made[space + 1] : 'B';
        if (actual != units[unit])
        {
                positive_into_human_1024_string(into, (positive)value);
                return;
        }

        if (space >= 2 && made[space - 2] == '.' && made[space - 1] == '0')
                space -= 2;
        memory_copy_apart(into, made, space);
        into[space++] = units[unit];
        into[space] = end;
}

static string_address tools_fincore_field(address_any row, p8 column,
                                          p8 address_to scratch)
{
        tools_fincore_row address_to entry = (tools_fincore_row address_to)row;

        if (column == TOOLS_FINCORE_FILE)
                return entry->path;

        p64 value = column == TOOLS_FINCORE_RES ? entry->resident
                    : column == TOOLS_FINCORE_PAGES ? entry->pages
                                                    : entry->size;
        if (tools_fincore_bytes || column == TOOLS_FINCORE_PAGES)
                positive_into_string(scratch, (positive)value);
        else
                tools_fincore_human(scratch, value);
        return scratch;
}

static bool tools_fincore_one(string_address path,
                              tools_fincore_row address_to row)
{
#if defined(LINUX)
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);
        if (handle < 0)
        {
                string_format(file_fail, "fincore: failed to open: %s: %s\n",
                              path, file_reason(handle));
                return false;
        }

        file_facts facts;
        bipolar told = file_look_code(handle, (string_address)"",
                                      AT_EMPTY_PATH, address_of facts);
        if (told < 0)
        {
                string_format(file_fail, "fincore: failed to stat: %s: %s\n",
                              path, file_reason(told));
                system_close(handle);
                return false;
        }
        if ((facts.mode & MODE_FORMAT) != MODE_FILE)
        {
                string_format(file_fail,
                              "fincore: not a regular file: %s\n", path);
                system_close(handle);
                return false;
        }

        positive page = system_page_size();
        p64 pages = facts.size / page + (facts.size % page != 0);
        p64 resident = 0;
        bool cache_answered = false;

        if (facts.size)
        {
                tools_fincore_range range = {0, facts.size};
                tools_fincore_cache cache = {0, 0, 0, 0, 0};
                bipolar cached = system_call_4(
                    syscall(cachestat), (positive)handle,
                    (positive)address_of range, (positive)address_of cache, 0);

                if (!cached)
                {
                        resident = cache.cached < pages ? cache.cached : pages;
                        cache_answered = true;
                }
        }

        if (facts.size && !cache_answered)
        {
                bipolar mapped = system_call_6(
                    syscall(mmap), 0, (positive)facts.size, FILE_PROTECT_NONE,
                    FILE_MAP_SHARED, (positive)handle, 0);
                if (mapped < 0)
                {
                        string_format(file_fail,
                                      "fincore: failed to mmap: %s: %s\n",
                                      path, file_reason(mapped));
                        system_close(handle);
                        return false;
                }

                p64 done = 0;
                p8 address_to vector = file_transfer;
                positive vector_room = sizeof(file_transfer);
                if (text_arena && text_arena_used < TEXT_ARENA_BYTES)
                {
                        vector = text_arena + text_arena_used;
                        vector_room = TEXT_ARENA_BYTES - text_arena_used;
                }
                while (done < pages)
                {
                        positive count = pages - done > vector_room
                            ? vector_room : (positive)(pages - done);
                        positive length = count * page;
                        p64 remaining = facts.size - done * page;
                        if (remaining < length)
                                length = (positive)remaining;

                        bipolar answer = system_call_3(
                            syscall(mincore),
                            (positive)((p8 address_to)(positive)mapped +
                                       done * page),
                            length, (positive)vector);
                        if (answer < 0)
                        {
                                string_format(file_fail,
                                              "fincore: failed to do mincore: %s: %s\n",
                                              path, file_reason(answer));
                                system_call_2(syscall(munmap),
                                              (positive)mapped,
                                              (positive)facts.size);
                                system_close(handle);
                                return false;
                        }
                        for (positive i = 0; i < count; i++)
                                resident += (vector[i] & 1) != 0;
                        done += count;
                }

                system_call_2(syscall(munmap), (positive)mapped,
                              (positive)facts.size);
        }
        system_close(handle);

        row->path = path;
        row->pages = resident;
        row->resident = resident * page;
        row->size = facts.size;
        return true;
#else
        (void)path;
        (void)row;
        string_format(file_fail, "fincore: mincore requires Linux\n");
        return false;
#endif
}

static b32 tools_fincore_main()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"fincore",
            .allowed = (string_address)"JbcnorRCAhV",
            .valued = (string_address)"o",
            .longs = tools_fincore_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (taking.flags & FILE_FLAG('h'))
                return ul_usage("fincore", "[options] file...");
        if (taking.flags & FILE_FLAG('V'))
        {
                string_format(log, "fincore from dawning-kit\n");
                log_flush();
                return 0;
        }
        if (!file_operand_count)
                return ul_bad_usage("fincore", "no file specified");
        if (taking.flags & FILE_FLAG('c'))
                return ul_bad_usage("fincore", "grand totals are not supported");
        if (taking.flags & FILE_FLAG('R'))
                return ul_bad_usage("fincore", "recursive traversal is not supported");
        if (taking.flags & FILE_FLAG('C'))
                return ul_bad_usage("fincore", "cachestat option is not supported");
        if (taking.flags & FILE_FLAG('A'))
                return ul_bad_usage("fincore", "extended output metadata is not supported");

        static const p8 defaults[] = {
            TOOLS_FINCORE_RES, TOOLS_FINCORE_PAGES,
            TOOLS_FINCORE_SIZE, TOOLS_FINCORE_FILE,
        };
        p8 columns[TOOLS_FINCORE_COLUMNS];
        positive column_count = 0;
        string_address output = file_option_value(address_of taking, 'o');
        if (output)
        {
                if (!ul_table_column_list(
                        output, tools_fincore_columns, TOOLS_FINCORE_COLUMNS,
                        defaults, array_count(defaults), columns,
                        address_of column_count))
                        return ul_bad_usage("fincore", "unknown output column");
        }
        else
                for (positive i = 0; i < array_count(defaults); i++)
                        columns[column_count++] = defaults[i];

        text_begin("fincore");
        text_arena_used = 0;
        if (file_operand_count > positive_max / sizeof(tools_fincore_row))
                return text_refuse(null, "too many files", 1);
        tools_fincore_row address_to rows =
            (tools_fincore_row address_to)text_arena_take(
                file_operand_count * sizeof(*rows));
        if (!rows)
                return text_done(1);

        positive count = 0;
        bool failed = false;
        for (positive i = 0; i < file_operand_count; i++)
        {
                tools_fincore_row row;
                if (tools_fincore_one(file_operand_at(i), address_of row))
                        rows[count++] = row;
                else
                        failed = true;
        }

        tools_fincore_bytes = (taking.flags & FILE_FLAG('b')) != 0;
        tools_fincore_columns[TOOLS_FINCORE_RES].width =
            tools_fincore_bytes ? 5 : 0;
        tools_fincore_columns[TOOLS_FINCORE_RES].json = tools_fincore_bytes
            ? UL_TABLE_NUMBER : UL_TABLE_STRING;
        tools_fincore_columns[TOOLS_FINCORE_SIZE].json = tools_fincore_bytes
            ? UL_TABLE_NUMBER : UL_TABLE_STRING;

        if (taking.flags & FILE_FLAG('J'))
                ul_table_json("fincore", rows, sizeof(*rows), count,
                              tools_fincore_columns, columns, column_count,
                              tools_fincore_field);
        else
                ul_table_out(rows, sizeof(*rows), count,
                             tools_fincore_columns, TOOLS_FINCORE_COLUMNS,
                             columns, column_count,
                             !(taking.flags & FILE_FLAG('n')),
                             (taking.flags & FILE_FLAG('r')) != 0,
                             tools_fincore_field);
        log_flush();
        return failed ? text_done(1) : 0;
}
