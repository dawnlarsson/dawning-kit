/*
        Experimental C standard library

        SNTP: RFC 5905 on-wire offset and delay, then a clock filter

        A single sample's offset error is (d_fwd - d_rev) / 2, and that is
        bounded by half the round-trip delay. Integer-second arithmetic and
        planting the server's transmit fraction as tv_nsec both throw that
        bound away. Five samples with the smallest delay kept is the RFC
        5905 clock filter, not an average: averaging a one-sided queue spike
        poisons the estimate.

        The clock is stepped by that offset with adjtimex ADJ_SETOFFSET, not
        by reading the clock again and planting a new wall time: the gap
        between those two traps would be extra error. A machine whose clock
        is still at the epoch must be allowed a decades-long first step; one
        whose clock already looks like a civil date may not jump more than a
        day, and a refresh of a synchronised clock may not jump more than
        two seconds. A kiss-o-death, a stratum 0, or a root delay/dispersion
        worse than a second abandons that server rather than collecting more
        samples from it.

        T1 is read, the originate stamp is written, and the packet is sent
        with nothing else in between. T4 is read the moment recv returns.
        The conversion of those timespecs into nanoseconds waits until after
        the trap. This tree has no vDSO: clock_gettime is a syscall, and
        that cost dwarfs the stores, so the C must not add a second one.

        Dawn Larsson - Apache 2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_SNTP
#define STANDARD_MODERN_C_NET_SNTP

#define SNTP_PORT 123
#define SNTP_PACKET 48
#define SNTP_SECONDS 2
#define SNTP_SAMPLES 5
#define SNTP_UNIX 2208988800u
#define SNTP_LI_VN_MODE 0x23
#define SNTP_NANOSECONDS 1000000000ull
#define SNTP_OK 0
#define SNTP_NO_SERVER (-1)
#define SNTP_NO_REPLY (-2)
#define SNTP_MALFORMED (-3)
#define SNTP_BAD_SERVER (-4)
#define SNTP_DELAY_MOST_NS ((bipolar)2 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_OFFSET_MOST_NS ((bipolar)24 * 3600 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_OFFSET_SYNCED_NS ((bipolar)2 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_WALL_LEAST 1577836800ll /* 2020-01-01 */
#define SNTP_WALL_MOST 2082758400ll  /* 2036-01-01 */
#define SNTP_WALL_LEAST_NS \
        ((bipolar)SNTP_WALL_LEAST * (bipolar)SNTP_NANOSECONDS)
#define SNTP_WALL_MOST_NS \
        ((bipolar)SNTP_WALL_MOST * (bipolar)SNTP_NANOSECONDS)
#define SNTP_TIMESPEC_SECONDS_MOST 9223372035ull
#define SNTP_ERA ((bipolar)4294967296)
#define SNTP_SHORT_SECOND 0x10000u
#define SNTP_TEST_NOW \
        ((bipolar)1700000000 * (bipolar)SNTP_NANOSECONDS)

typedef struct
{
        bipolar offset_ns;
        bipolar delay_ns;
        bool ok;
} sntp_sample;

static inline INLINE CONST bool sntp_wall_ok(bipolar ns)
{
        return ns >= SNTP_WALL_LEAST_NS && ns <= SNTP_WALL_MOST_NS;
}

/*
        A local stamp may sit anywhere from the epoch to the end of the
        window, because a machine that has never been told the time boots
        at zero. It may not sit past the window: t1 and t4 are the only
        two terms sntp_offset_delay adds that are not already bounded by
        the wire format, and an unbounded one overflows the sum.
*/
static inline INLINE CONST bool sntp_local_ok(bipolar ns)
{
        return ns >= 0 && ns <= SNTP_WALL_MOST_NS;
}

static inline INLINE CONST bipolar sntp_timespec_ns(p64 seconds, p64 nanoseconds)
{
        if (nanoseconds >= SNTP_NANOSECONDS)
                return -1;
        if (seconds > SNTP_TIMESPEC_SECONDS_MOST)
                return -1;
        return (bipolar)seconds * (bipolar)SNTP_NANOSECONDS +
               (bipolar)nanoseconds;
}

static inline INLINE bipolar sntp_now_ns(void)
{
        p64 now[2] = {0, 0};

        if (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                          (positive)now) < 0)
                return -1;
        return sntp_timespec_ns(now[0], now[1]);
}

static inline INLINE fn sntp_put_stamp(p8 address_to field, p64 unix_seconds,
                                       p64 unix_nsec)
{
        p32 ntp_seconds = (p32)(unix_seconds + SNTP_UNIX);
        p32 ntp_frac =
            (p32)(((p64)unix_nsec << 32) / SNTP_NANOSECONDS);

        network_store_32(field, ntp_seconds);
        network_store_32(field + 4, ntp_frac);
}

static inline INLINE PURE bipolar sntp_load_stamp(p8 address_to field)
{
        p32 ntp_seconds = network_load_32(field);
        p32 ntp_frac = network_load_32(field + 4);
        bipolar unix_seconds = (bipolar)ntp_seconds - (bipolar)SNTP_UNIX;
        bipolar unix_nsec =
            (bipolar)(((p64)ntp_frac * SNTP_NANOSECONDS) >> 32);

        if (!(ntp_seconds & 0x80000000u))
                unix_seconds += SNTP_ERA;
        return unix_seconds * (bipolar)SNTP_NANOSECONDS + unix_nsec;
}

static inline INLINE CONST bool sntp_short_ok(p32 word)
{
        return !(word & 0x80000000u) && word <= SNTP_SHORT_SECOND;
}

static COLD fn sntp_split_offset(bipolar ns, bipolar address_to seconds,
                            bipolar address_to nanoseconds)
{
        bipolar sec = ns / (bipolar)SNTP_NANOSECONDS;
        bipolar nsec = ns % (bipolar)SNTP_NANOSECONDS;

        if (nsec < 0)
        {
                sec -= 1;
                nsec += (bipolar)SNTP_NANOSECONDS;
        }
        address_to seconds = sec;
        address_to nanoseconds = nsec;
}

static inline INLINE fn sntp_offset_delay(bipolar t1, bipolar t2, bipolar t3,
                                          bipolar t4,
                                          bipolar address_to offset_ns,
                                          bipolar address_to delay_ns)
{
        address_to offset_ns = ((t2 - t1) + (t3 - t4)) / 2;
        address_to delay_ns = (t4 - t1) - (t3 - t2);
}

static CONST COLD bool sntp_sample_sane(bipolar t1, bipolar t2, bipolar t3,
                                   bipolar t4, bipolar offset_ns,
                                   bipolar delay_ns, bool tight)
{
        if (t1 < 0 || t4 < t1 || t3 < t2)
                return false;
        if (!sntp_wall_ok(t2) || !sntp_wall_ok(t3))
                return false;
        if (delay_ns < 0 || delay_ns > SNTP_DELAY_MOST_NS)
                return false;
        if (tight)
        {
                if (offset_ns < -SNTP_OFFSET_SYNCED_NS ||
                    offset_ns > SNTP_OFFSET_SYNCED_NS)
                        return false;
        }
        else if (sntp_wall_ok(t1))
        {
                if (offset_ns < -SNTP_OFFSET_MOST_NS ||
                    offset_ns > SNTP_OFFSET_MOST_NS)
                        return false;
        }
        return true;
}

static COLD bool sntp_target_ok(bipolar now, bipolar offset_ns,
                           bipolar address_to target)
{
        if (offset_ns > 0 && now > bipolar_max - offset_ns)
                return false;
        if (offset_ns < 0 && now < bipolar_min - offset_ns)
                return false;
        address_to target = now + offset_ns;
        return sntp_wall_ok(address_to target);
}

static PURE COLD bipolar sntp_pick(sntp_sample address_to row, positive count)
{
        bipolar best = -1;
        positive at;

        for (at = 0; at < count; at++)
                if (row[at].ok &&
                    (best < 0 || row[at].delay_ns < row[best].delay_ns ||
                     (row[at].delay_ns == row[best].delay_ns &&
                      (bipolar)at > best)))
                        best = (bipolar)at;
        return best;
}

static COLD bool sntp_math_ok(void)
{
        bipolar offset = 0;
        bipolar delay = 0;
        bipolar target = 0;
        bipolar two_days = (bipolar)2 * 86400 * (bipolar)SNTP_NANOSECONDS;
        positive at;
        sntp_sample row[5] = {
            {10000000, 20000000, true}, {8000000, 80000000, true},
            {2000000, 15000000, true},  {4000000, 40000000, true},
            {50000000, 200000000, true},
        };
        static const bipolar delay_case[][6] = {
            {0, 1000000000, 1000000000, 2000000000, 0, 2000000000},
            {0, 1050000000, 1050000000, 2000000000, 50000000, 2000000000},
            {0, 100000000, 100000000, 1100000000, -450000000, 1100000000},
        };
        static const struct
        {
                p32 seconds;
                p32 fraction;
                bipolar want;
        } stamp_case[] = {
            /* era 0, the high bit set: 1968 through February 2036 */
            {SNTP_UNIX, 0, 0},
            {SNTP_UNIX + 1, 0, (bipolar)SNTP_NANOSECONDS},
            {SNTP_UNIX, 0x80000000u, 500000000},
            /* era 1, the high bit clear: February 2036 onward */
            {0, 0, (bipolar)2085978496 * (bipolar)SNTP_NANOSECONDS},
            {1, 0, (bipolar)2085978497 * (bipolar)SNTP_NANOSECONDS},
        };
        static const bipolar split_case[][3] = {
            {1500000000, 1, 500000000},
            {-1500000000, -2, 500000000},
            {-1, -1, 999999999},
            {0, 0, 0},
        };
        static const struct
        {
                bipolar t1;
                bipolar t2;
                bipolar t3;
                bipolar t4;
                bipolar off;
                bipolar del;
                bool tight;
                bool want;
        } sane_case[] = {
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 2000000000, 0,
             2000000000, false, true},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 2000000000, 0,
             2000000000, true, true},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 3000000001, 0,
             3000000001, false, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 500000000, 0,
             -500000000, false, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 3000000000,
             SNTP_TEST_NOW + 3000000000, SNTP_TEST_NOW + 50000000, 2975000000,
             50000000, true, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 2000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 50000000, 0, 50000000,
             false, false},
        };

        for (at = 0; at < array_count(delay_case); at++)
        {
                sntp_offset_delay(delay_case[at][0], delay_case[at][1],
                                  delay_case[at][2], delay_case[at][3],
                                  address_of offset, address_of delay);
                if (offset != delay_case[at][4] || delay != delay_case[at][5])
                        return false;
        }

        for (at = 0; at < array_count(sane_case); at++)
                if (sntp_sample_sane(sane_case[at].t1, sane_case[at].t2,
                                     sane_case[at].t3, sane_case[at].t4,
                                     sane_case[at].off, sane_case[at].del,
                                     sane_case[at].tight) != sane_case[at].want)
                        return false;

        sntp_offset_delay(0, SNTP_TEST_NOW + 1000000000,
                          SNTP_TEST_NOW + 1000000000, 100000000,
                          address_of offset, address_of delay);
        if (!sntp_sample_sane(0, SNTP_TEST_NOW + 1000000000,
                              SNTP_TEST_NOW + 1000000000, 100000000, offset,
                              delay, false) ||
            sntp_sample_sane(0, SNTP_TEST_NOW + 1000000000,
                             SNTP_TEST_NOW + 1000000000, 100000000, offset,
                             delay, true) ||
            !sntp_target_ok(0, offset, address_of target) ||
            target < SNTP_TEST_NOW || target > SNTP_TEST_NOW + 1000000000)
                return false;

        sntp_offset_delay(SNTP_TEST_NOW, SNTP_TEST_NOW + two_days,
                          SNTP_TEST_NOW + two_days,
                          SNTP_TEST_NOW + 50000000, address_of offset,
                          address_of delay);
        if (sntp_sample_sane(SNTP_TEST_NOW, SNTP_TEST_NOW + two_days,
                             SNTP_TEST_NOW + two_days,
                             SNTP_TEST_NOW + 50000000, offset, delay, false))
                return false;

        if (!sntp_short_ok(0) || !sntp_short_ok(SNTP_SHORT_SECOND) ||
            sntp_short_ok(SNTP_SHORT_SECOND + 1) ||
            sntp_short_ok(0x80000000u))
                return false;

        /*
                The seconds bound is the one that has to hold exactly: a
                whole second short of it, with the largest fraction, is
                still a number, and one second past it is not.
        */
        if (sntp_timespec_ns(SNTP_TIMESPEC_SECONDS_MOST,
                             SNTP_NANOSECONDS - 1) < 0 ||
            sntp_timespec_ns(SNTP_TIMESPEC_SECONDS_MOST + 1, 0) >= 0 ||
            sntp_timespec_ns(0, SNTP_NANOSECONDS) >= 0)
                return false;

        if (!sntp_local_ok(0) || !sntp_local_ok(SNTP_WALL_MOST_NS) ||
            sntp_local_ok(SNTP_WALL_MOST_NS + 1) || sntp_local_ok(-1))
                return false;

        for (at = 0; at < array_count(stamp_case); at++)
        {
                p8 field[8];

                network_store_32(field, stamp_case[at].seconds);
                network_store_32(field + 4, stamp_case[at].fraction);
                if (sntp_load_stamp(field) != stamp_case[at].want)
                        return false;
        }

        for (at = 0; at < array_count(split_case); at++)
        {
                sntp_split_offset(split_case[at][0], address_of offset,
                                  address_of delay);
                if (offset != split_case[at][1] || delay != split_case[at][2])
                        return false;
        }

        return sntp_pick(row, 5) == 2 && row[2].offset_ns == 2000000;
}

static HOT bipolar sntp_exchange(b32 handle,
                                 network_deadline address_to deadline,
                                 bool tight, sntp_sample address_to into)
{
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        p64 sent[2];
        p64 got[2];
        bipolar t1;
        bipolar t2;
        bipolar t3;
        bipolar t4;
        bipolar wait;
        bipolar received;
        bipolar offset = 0;
        bipolar delay = 0;

        into->ok = false;
        memory_fill(request, 0, sizeof(request));
        request[0] = SNTP_LI_VN_MODE;

        if_rare (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                               (positive)sent) < 0)
                return SNTP_NO_REPLY;
        sntp_put_stamp(request + 40, sent[0], sent[1]);
        if_rare (socket_send(handle, request, SNTP_PACKET, 0, 0, 0) < 0)
                return SNTP_NO_REPLY;
        t1 = sntp_timespec_ns(sent[0], sent[1]);
        if_rare (!sntp_local_ok(t1))
                return SNTP_NO_REPLY;

        for (;;)
        {
                wait = network_wait_readable_until(handle, deadline);
                if_rare (wait <= 0)
                        return SNTP_NO_REPLY;
                received = socket_receive(handle, reply, sizeof(reply), 0, 0,
                                          0);
                if (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                                  (positive)got) < 0)
                        return SNTP_NO_REPLY;
                if_rare (received < SNTP_PACKET)
                        continue;
                if_rare (memory_compare(reply + 24, request + 40, 8))
                        continue;
                if_rare ((reply[0] >> 6) == 3)
                        return SNTP_BAD_SERVER;
                if_rare ((reply[0] & 0x7) != 4 || !reply[1] || reply[1] >= 16)
                        return SNTP_BAD_SERVER;
                if_rare (!sntp_short_ok(network_load_32(reply + 4)) ||
                         !sntp_short_ok(network_load_32(reply + 8)))
                        return SNTP_BAD_SERVER;
                t4 = sntp_timespec_ns(got[0], got[1]);
                if_rare (!sntp_local_ok(t4))
                        return SNTP_MALFORMED;
                t2 = sntp_load_stamp(reply + 32);
                t3 = sntp_load_stamp(reply + 40);
                sntp_offset_delay(t1, t2, t3, t4, address_of offset,
                                  address_of delay);
                if_rare (!sntp_sample_sane(t1, t2, t3, t4, offset, delay,
                                           tight))
                        return SNTP_MALFORMED;
                into->offset_ns = offset;
                into->delay_ns = delay;
                into->ok = true;
                return SNTP_OK;
        }
}

static COLD bipolar sntp_query_at(p32 server, bool filter, bool tight,
                             bipolar address_to offset_ns)
{
        socket_address_internet where = {
            .family = AF_INET,
            .port = network_order_16(SNTP_PORT),
            .host = network_order_32(server),
        };
        network_deadline deadline;
        sntp_sample row[SNTP_SAMPLES];
        positive want = filter ? SNTP_SAMPLES : 1;
        positive at;
        bipolar handle;
        bipolar best;
        bipolar failed = SNTP_NO_REPLY;

        if (!sntp_math_ok())
                return SNTP_MALFORMED;

        if (!network_deadline_begin(address_of deadline,
                                    SNTP_SECONDS * (filter ? SNTP_SAMPLES : 1),
                                    0))
                return SNTP_NO_REPLY;

        handle = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return SNTP_NO_SERVER;
        if (socket_connect((b32)handle, address_of where, sizeof(where)) < 0)
        {
                socket_close((b32)handle);
                return SNTP_NO_SERVER;
        }

        memory_zero(row, sizeof(row));
        for (at = 0; at < want; at++)
        {
                failed = sntp_exchange((b32)handle, address_of deadline, tight,
                                       row + at);
                if (failed == SNTP_BAD_SERVER)
                        break;
        }
        socket_close((b32)handle);

        best = sntp_pick(row, want);
        if (best < 0)
                return failed < 0 ? failed : SNTP_NO_REPLY;
        address_to offset_ns = row[best].offset_ns;
        return SNTP_OK;
}

static COLD bipolar sntp_query(string_address name, bool filter, bool tight,
                          bipolar address_to offset_ns)
{
        bipolar numeric;
        p32 host = 0;
        bipolar found;

        if (!name || !name[0])
                return SNTP_NO_SERVER;
        numeric = string_to_host(name);
        if (numeric >= 0)
                return sntp_query_at((p32)numeric, filter, tight, offset_ns);

        found = dns_resolve_any((string_address) "/etc/resolv.conf", name,
                                address_of host, SNTP_SECONDS);
        if (found != DNS_OK)
                return SNTP_NO_SERVER;
        return sntp_query_at(host, filter, tight, offset_ns);
}

#endif
