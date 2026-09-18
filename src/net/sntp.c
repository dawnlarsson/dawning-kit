/*
        Experimental C standard library

        SNTP: one UDP query, the clock set

        Dawn Larsson - Apache 2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_SNTP
#define STANDARD_MODERN_C_NET_SNTP

#define SNTP_PORT 123
#define SNTP_PACKET 48
#define SNTP_SECONDS 2
#define SNTP_UNIX 2208988800u
#define SNTP_LI_VN_MODE 0x23
#define SNTP_OK 0
#define SNTP_NO_SERVER (-1)
#define SNTP_NO_REPLY (-2)
#define SNTP_MALFORMED (-3)

static p32 sntp_unix_seconds(p8 address_to field)
{
        return network_load_32(field) - SNTP_UNIX;
}

static p32 sntp_unix_nanoseconds(p8 address_to field)
{
        return (p32)(((p64)network_load_32(field) * 1000000000ull) >> 32);
}

static bipolar sntp_query_at(p32 server, p64 address_to seconds,
                             p32 address_to nanoseconds)
{
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        socket_address_internet where = {
            .family = AF_INET,
            .port = network_order_16(SNTP_PORT),
            .host = network_order_32(server),
        };
        network_deadline deadline;
        p64 sent[2] = {0, 0};
        p64 got[2] = {0, 0};
        bipolar handle;
        bipolar wait;
        bipolar received;
        p32 t2;
        p32 t3;
        bipolar offset;

        memory_fill(request, 0, sizeof(request));
        request[0] = SNTP_LI_VN_MODE;
        if (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                          (positive)sent) >= 0)
        {
                network_store_32(request + 40, (p32)((p64)sent[0] + SNTP_UNIX));
                network_store_32(request + 44,
                                 (p32)(((p64)sent[1] << 32) / 1000000000ull));
        }

        if (!network_deadline_begin(address_of deadline, SNTP_SECONDS, 0))
                return SNTP_NO_REPLY;

        handle = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return SNTP_NO_SERVER;
        if (socket_connect((b32)handle, address_of where, sizeof(where)) < 0)
        {
                socket_close((b32)handle);
                return SNTP_NO_SERVER;
        }
        if (socket_send((b32)handle, request, SNTP_PACKET, 0, 0, 0) < 0)
        {
                socket_close((b32)handle);
                return SNTP_NO_REPLY;
        }

        wait = network_wait_readable_until((b32)handle, address_of deadline);
        if (wait <= 0)
        {
                socket_close((b32)handle);
                return SNTP_NO_REPLY;
        }
        received = socket_receive((b32)handle, reply, sizeof(reply), 0, 0, 0);
        system_call_2(syscall(clock_gettime), CLOCK_REALTIME, (positive)got);
        socket_close((b32)handle);
        if (received < SNTP_PACKET)
                return SNTP_MALFORMED;
        if ((reply[0] >> 6) == 3)
                return SNTP_MALFORMED;
        if ((reply[0] & 0x7) != 4 || !reply[1] || reply[1] >= 16)
                return SNTP_MALFORMED;

        t2 = sntp_unix_seconds(reply + 32);
        t3 = sntp_unix_seconds(reply + 40);
        offset = (((bipolar)t2 - (bipolar)sent[0]) +
                  ((bipolar)t3 - (bipolar)got[0])) /
                 2;
        address_to seconds = (p64)((bipolar)got[0] + offset);
        address_to nanoseconds = sntp_unix_nanoseconds(reply + 44);
        return SNTP_OK;
}

static bipolar sntp_query(string_address name, p64 address_to seconds,
                          p32 address_to nanoseconds)
{
        bipolar numeric;
        p32 host = 0;
        bipolar found;

        if (!name || !name[0])
                return SNTP_NO_SERVER;
        numeric = string_to_host(name);
        if (numeric >= 0)
                return sntp_query_at((p32)numeric, seconds, nanoseconds);

        found = dns_resolve_any((string_address) "/etc/resolv.conf", name,
                                address_of host, SNTP_SECONDS);
        if (found != DNS_OK)
                return SNTP_NO_SERVER;
        return sntp_query_at(host, seconds, nanoseconds);
}

#endif
