/*
        Timezone, NTP and keyboard layout, as moonwater verbs.

        Choices live on /root so an image update keeps them. The machine
        starts NTP itself: restore forks the first query before init, and the
        wait loop keeps walking servers until the clock is set. Five samples
        keep the lowest delay unless /root/ntp.filter says off. The kernel
        adds that offset with adjtimex; a kiss-o-death drops the server.

        The forked query is reaped with wait4, not asked after with kill.
        A pid that has exited but not been waited for is still a pid, so
        kill(pid, 0) answers zero for a zombie exactly as it does for a
        live child: the poll would see its first query running for ever,
        never retry a boot that failed for want of a network, and never
        poll again. wait4 is the only call that distinguishes the two.

        NOTHING BELOW HAS BEEN SEEN TO SET A CLOCK

        Setting CLOCK_REALTIME needs a machine it is acceptable to
        disturb, and no machine in reach was one, so neither the step nor
        the slew has ever been run against a kernel that carried it out.
        What has been checked is what gets asked for: every ADJ_ and STA_
        value here against uapi/linux/timex.h, every timex word index
        against the struct's own offsets, and the choice between stepping
        and slewing against crafted offsets either side of the threshold
        in the machine lane. Take that as the request being right, not as
        the request having been granted.

        The reason to be careful about the difference is in the constants
        below. ADJ_SETOFFSET was 0x80, which is ADJ_TAI, for as long as
        this file has had it -- so no correction this program ever
        computed reached the clock. It survived because adjtimex answers a
        wrong mode the same way it answers a right one: with the clock
        state, a non-negative number the caller reads as success. It also
        cleared STA_UNSYNC on the way past, so the machine went on to
        report itself synchronised. Anyone adding a mode here should know
        that a non-negative return proves the call was accepted and
        nothing else, and that there is no test that can tell you
        otherwise, because nothing unprivileged can ask the kernel to
        demonstrate which mode a bit meant.
*/

/*      ----------------------------------------------------------------
        SNTP: how the clock above is asked what the time is.
        ---------------------------------------------------------------- */

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

        SNTP_WALL_MOST ends that window at 2036-01-01, which is before the
        NTP era rolls over rather than because of it: sntp_load_stamp reads
        the era from the top of the seconds field and converts either one.
        Moving the window is therefore the single edit that date needs, and
        it is safe to move. The offset sum is two differences of four terms
        the window bounds, and carrying the window all the way to 2104, the
        end of era 1, leaves that sum at 8.47 of the 9.22 the type holds.

        T1 is read, the originate stamp is written, and the packet is sent
        with nothing else in between. T4 is read the moment recv returns.
        The conversion of those timespecs into nanoseconds waits until after
        the trap. This tree has no vDSO: clock_gettime is a syscall, and
        that cost dwarfs the stores, so the C must not add a second one.

        WHAT HAS BEEN MEASURED, AND WHAT HAS NOT

        Against five servers at once, with the box's own clock held
        synchronised by something else as the reference, this file's
        answers sat within 193 microseconds of it; a single-sample client
        with userspace stamps, asked the same servers in the same minute,
        spread to 1207. Both agree on sign and scale, so the difference
        is the five-sample filter and the kernel stamps, not a disagreement
        about what time it is.

        The two kernel stamps are worth, at the median of real exchanges:
        10355 ns for the arrival stamp, and 1729 ns to one server and 2320
        to another for the departure stamp. Both are one-sided, which is
        why they land in the offset at all -- the formula assumes the path
        is symmetric. Round-trip asymmetry itself cannot be measured from
        one end and so cannot be corrected here; across the servers above
        it accounts for a spread of several milliseconds, which is larger
        than everything this file does about anything else.

        What is not proven is the clock being set. Nothing here can take
        CLOCK_REALTIME on a machine that is not ours to disturb, so the
        step and slew paths in locale.c have never been executed against a
        kernel that carried them out. What is checked is the request: the
        mode words against uapi/linux/timex.h, and the decision between
        stepping and slewing against crafted offsets in the machine lane.
        A reader should take "the right thing is asked for" from this and
        not "the asking has been seen to work".

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
#define SNTP_RATE_LIMITED (-5)
#define SNTP_KISS_RATE 0x52415445u /* "RATE" */
#define SNTP_KISS_DENY 0x44454e59u /* "DENY" */
#define SNTP_KISS_RSTR 0x52535452u /* "RSTR" */
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
#define SNTP_RANDOM_NONBLOCK 1
#define SNTP_TIMESTAMPNS 35
#define SNTP_TIMESTAMPING 37
#define SNTP_TIMESTAMPING_WANT 2194u /* TX_SOFTWARE|SOFTWARE|OPT_ID|TSONLY */
#define SNTP_ERRQUEUE 0x2000
#define SNTP_DONTWAIT 0x40
#define SNTP_MESSAGE_WORDS 7
#define SNTP_CONTROL_WORDS 16
#define SNTP_CONTROL_HEAD (sizeof(positive) + 8)
#define SNTP_ERRQUEUE_MOST 4
#define SNTP_SOL_IP 0
#define SNTP_IP_RECVERR 11
#define SNTP_ERROR_TIMESTAMPING 4 /* SO_EE_ORIGIN_TIMESTAMPING */
#define SNTP_ERROR_BYTES 16       /* struct sock_extended_err */
#define SNTP_ERROR_ORIGIN 4       /* ee_origin within it */
#define SNTP_ERROR_SEQUENCE 12    /* ee_data, which carries the id */
#define SNTP_CONTROL_DATA                          \
        ((SNTP_CONTROL_HEAD + sizeof(positive) - 1) & \
         ~(sizeof(positive) - 1))
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

/*
        The transmit stamp goes out to be echoed back, and the echo is the
        only thing telling us a reply is ours. Sending the clock puts a
        number an off-path attacker can estimate into the one field it has
        to guess, and its low half is the whole of the guess: the seconds
        it already knows.

        Nothing reads this field back. t1 is taken from the timespec the
        trap filled, never from the packet, so the fraction carries no
        accuracy and a random one costs none. Thirty-two bits of it, on
        top of the source port the connected socket already randomises,
        is what the forgery has to match. The clock's own fraction is the
        fallback if the pool has no bytes to give, which is where this
        started.
*/
static inline INLINE fn sntp_put_stamp(p8 address_to field, p64 unix_seconds,
                                       p64 unix_nsec)
{
        p32 ntp_seconds = (p32)(unix_seconds + SNTP_UNIX);
        p32 ntp_frac =
            (p32)(((p64)unix_nsec << 32) / SNTP_NANOSECONDS);

        if (system_random_fill(address_of ntp_frac, sizeof(ntp_frac),
                               SNTP_RANDOM_NONBLOCK) < 0)
                ntp_frac = (p32)(((p64)unix_nsec << 32) / SNTP_NANOSECONDS);

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

/*
        t4 is meant to be when the reply arrived. Read after recv returns
        it is when this process next ran, which is the arrival plus however
        long the packet waited in the socket and however long the scheduler
        took to wake us. Half of that lands in the offset, and on an idle
        machine talking to a real server it measured a shade over eleven
        microseconds -- four hundred times the whole of the arithmetic that
        follows, and nothing the arithmetic can do anything about.

        SO_TIMESTAMPNS makes the kernel record the arrival in the softirq
        that takes the packet off the device and hand it over as a control
        message. Reading it needs recvmsg rather than recvfrom, and the
        socket calls in library.c are recvfrom, so the trap is made here
        the way this file already traps for clock_gettime.

        msghdr is seven pointer-width words -- the name and its length,
        the vector and its count, the control buffer and its length, and
        the flags -- which is its shape on every target this tree builds.
        A control message is a pointer-width length, then a level and a
        type of four bytes each, then the payload at the next word.

        Nothing here is required to work. A kernel that refuses the option
        or a path that delivers no control message leaves the stamp unset,
        and the caller reads the clock itself exactly as before.
*/
/*
        The walk is apart from the trap because it is the half that fails
        quietly: a wrong offset finds no message, and a receive that found
        no message is indistinguishable from a kernel that sent none. That
        reads as "the timestamp did not help" rather than as a mistake, so
        it is reached here by its own name and sntp_math_ok hands it
        buffers laid out by hand -- including ones whose length words lie.

        Every field is read from a buffer the kernel filled, and the two
        lengths are believed only as far as the buffer goes: a message
        claiming to be longer than what is left ends the walk.
*/
static bool sntp_control_stamp(p8 address_to control, positive length,
                               b32 kind, p64 address_to arrived)
{
        positive at = 0;

        while (at + SNTP_CONTROL_DATA <= length)
        {
                positive size = address_to(positive address_to)(control + at);
                b32 level = address_to(b32 address_to)(control + at +
                                                       sizeof(positive));
                b32 type = address_to(b32 address_to)(control + at +
                                                      sizeof(positive) + 4);

                if (size < SNTP_CONTROL_DATA || size > length - at)
                        break;
                if (level == SOL_SOCKET && type == kind &&
                    size - SNTP_CONTROL_DATA >= 2 * sizeof(p64))
                {
                        arrived[0] = address_to(p64 address_to)(
                            control + at + SNTP_CONTROL_DATA);
                        arrived[1] = address_to(p64 address_to)(
                            control + at + SNTP_CONTROL_DATA + sizeof(p64));
                        return true;
                }
                at += (size + sizeof(positive) - 1) & ~(sizeof(positive) - 1);
        }
        return false;
}

static HOT bipolar sntp_receive_stamped(b32 handle, p8 address_to reply,
                                        positive room,
                                        p64 address_to arrived,
                                        bool address_to stamped)
{
        positive message[SNTP_MESSAGE_WORDS];
        positive vector[2];
        positive control[SNTP_CONTROL_WORDS];
        bipolar got;

        address_to stamped = false;
        memory_zero(message, sizeof(message));
        memory_zero(control, sizeof(control));
        vector[0] = (positive)reply;
        vector[1] = room;
        message[2] = (positive)vector;
        message[3] = 1;
        message[4] = (positive)control;
        message[5] = sizeof(control);

        got = system_call_3(syscall(recvmsg), (positive)handle,
                            (positive)message, SNTP_DONTWAIT);
        if_rare (got < 0)
                return got;

        address_to stamped = sntp_control_stamp((p8 address_to)control,
                                                message[5], SNTP_TIMESTAMPNS,
                                                arrived);
        return got;
}

/*
        t1 has the same trouble t4 had, at the other end. It is read
        before the send trap, so it is the moment before the kernel is
        entered, and the packet leaves after the protocol stack has run.
        The offset formula assumes the two directions are symmetric, so a
        head start on the send side alone goes straight into the answer at
        half its size: measured against the kernel's own departure stamp,
        a median of 1729 ns to one server and 2320 ns to another.

        SOF_TIMESTAMPING_TX_SOFTWARE records the moment the packet is
        given to the driver and queues it on the socket's error queue.
        Measured on a real route it is already there when send returns, 50
        times out of 50, so one recvmsg that refuses to wait collects it
        and no poll is needed.

        Draining it is not optional once the option is on. A socket with
        anything on its error queue reports POLLERR, and the wait below
        asks about readability and would be woken by that for ever. One
        non-blocking read after each send empties it, which 100 exchanges
        across two servers confirm: no POLLERR survived into the wait.

        OPT_TSONLY keeps the packet itself off the queue, so what comes
        back is the timestamp and the error header beside it. The walk
        steps over anything that is not the timestamp, which is what lets
        a real ICMP error sit there without being mistaken for one.
*/
/*
        OPT_ID is already asked for, so the kernel numbers every transmit
        stamp with a counter that starts at zero when the option is set
        and rises by one per send. Five sends on one socket came back 0,
        1, 2, 3, 4.

        The number does not travel in the timestamp. It is in the error
        header beside it, as ee_data, and the same header says in
        ee_origin whether this queue entry is a timestamp at all or a
        real ICMP error that happens to be sitting there. Reading both is
        what makes the stamp provably the one belonging to the send being
        timed, rather than whichever stamp was on the queue -- a
        distinction that only bites if a drain is ever missed, which is
        exactly the case that cannot be tested from outside.

        A kernel that sends no error header, or one whose numbering does
        not line up, leaves the stamp unclaimed and the exchange falls
        back to the userspace reading, as it does when the option is
        refused outright.
*/
static bool sntp_control_sequence(p8 address_to control, positive length,
                                  p32 address_to sequence)
{
        positive at = 0;

        while (at + SNTP_CONTROL_DATA <= length)
        {
                positive size = address_to(positive address_to)(control + at);
                b32 level = address_to(b32 address_to)(control + at +
                                                       sizeof(positive));
                b32 type = address_to(b32 address_to)(control + at +
                                                      sizeof(positive) + 4);

                if (size < SNTP_CONTROL_DATA || size > length - at)
                        break;
                if (level == SNTP_SOL_IP && type == SNTP_IP_RECVERR &&
                    size - SNTP_CONTROL_DATA >= SNTP_ERROR_BYTES)
                {
                        p8 address_to body = control + at + SNTP_CONTROL_DATA;

                        if (body[SNTP_ERROR_ORIGIN] == SNTP_ERROR_TIMESTAMPING)
                        {
                                address_to sequence =
                                    address_to(p32 address_to)(
                                        body + SNTP_ERROR_SEQUENCE);
                                return true;
                        }
                }
                at += (size + sizeof(positive) - 1) & ~(sizeof(positive) - 1);
        }
        return false;
}

static HOT bool sntp_transmit_stamp(b32 handle, p32 wanted,
                                    p64 address_to departed)
{
        positive message[SNTP_MESSAGE_WORDS];
        positive vector[2];
        positive control[SNTP_CONTROL_WORDS];
        p8 sink[SNTP_PACKET];
        p64 stamp[2];
        p32 sequence;
        bool found = false;
        positive round;

        for (round = 0; round < SNTP_ERRQUEUE_MOST; round++)
        {
                bipolar got;

                sequence = 0;
                memory_zero(message, sizeof(message));
                memory_zero(control, sizeof(control));
                vector[0] = (positive)sink;
                vector[1] = sizeof(sink);
                message[2] = (positive)vector;
                message[3] = 1;
                message[4] = (positive)control;
                message[5] = sizeof(control);
                got = system_call_3(syscall(recvmsg), (positive)handle,
                                    (positive)message,
                                    SNTP_ERRQUEUE | SNTP_DONTWAIT);
                if (got < 0)
                        break;
                if (sntp_control_stamp((p8 address_to)control, message[5],
                                       SNTP_TIMESTAMPING, stamp) &&
                    sntp_control_sequence((p8 address_to)control, message[5],
                                          address_of sequence) &&
                    sequence == wanted)
                {
                        departed[0] = stamp[0];
                        departed[1] = stamp[1];
                        found = true;
                }
        }
        return found;
}

/*
        Everything below is read out of forty-eight bytes a stranger sent.
        The socket is connected, so the kernel has already dropped a
        datagram whose source is not the server's, but an on-path answer
        and a blind one aimed at an open port both arrive here.

        The origin stamp is the check that carries the weight: it is the
        transmit stamp we planted, echoed back, and an answer that does
        not carry it was not an answer to our question. It is dropped and
        the wait resumes rather than ending the exchange, because a late
        reply to an earlier sample is not a reason to give up on this one.

        Stratum zero is a kiss-o-death and the four bytes at 12 say which.
        RATE is the server asking to be asked less often, which is a
        different answer from DENY and RSTR: it is reported separately so
        the policy above can wait instead of walking to the next server
        and asking again immediately.
*/
static COLD bipolar sntp_reply_ok(p8 address_to reply, p8 address_to request)
{
        if (memory_compare(reply + 24, request + 40, 8))
                return SNTP_NO_REPLY;
        if ((reply[0] & 0x7) != 4)
                return SNTP_BAD_SERVER;
        if (!reply[1])
                return network_load_32(reply + 12) == SNTP_KISS_RATE
                           ? SNTP_RATE_LIMITED
                           : SNTP_BAD_SERVER;
        if ((reply[0] >> 6) == 3 || reply[1] >= 16)
                return SNTP_BAD_SERVER;
        if (!sntp_short_ok(network_load_32(reply + 4)) ||
            !sntp_short_ok(network_load_32(reply + 8)))
                return SNTP_BAD_SERVER;
        return SNTP_OK;
}

static COLD bool sntp_math_ok(void)
{
        bipolar offset = 0;
        bipolar delay = 0;
        bipolar target = 0;
        bipolar two_days = (bipolar)2 * 86400 * (bipolar)SNTP_NANOSECONDS;
        positive at;
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        p8 control[96];
        p64 arrived[2];
        p32 sequence;
        static const struct
        {
                positive claimed; /* what the message says its length is */
                b32 level;
                b32 kind;
                positive held;    /* what the buffer actually holds */
                bool want;
        } control_case[] = {
            /* the message the kernel really sends */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, true},
            /* some other control message, of which there are many */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS + 1,
             SNTP_CONTROL_DATA + 16, false},
            {SNTP_CONTROL_DATA + 16, 0, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* a length word smaller than the header it heads */
            {SNTP_CONTROL_DATA - 8, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* a length word reaching past the end of the buffer */
            {SNTP_CONTROL_DATA + 64, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* the right message with too little room for a timespec */
            {SNTP_CONTROL_DATA + 8, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 8, false},
            /* nothing at all, which is what a kernel without the option
               sends, and the case the caller falls back on */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS, 0, false},
        };
        static const struct
        {
                p8 first;
                p8 stratum;
                p32 root_delay;
                p32 root_dispersion;
                p32 reference_id;
                bool echoed;
                bipolar want;
        } reply_case[] = {
            /* a stratum 2 server answering the question we asked */
            {0x24, 2, 0, 0, 0, true, SNTP_OK},
            /* the same packet with the origin stamp not echoed: a forgery,
               and the one check standing between us and an off-path lie */
            {0x24, 2, 0, 0, 0, false, SNTP_NO_REPLY},
            /* mode 3 is a request, not a reply */
            {0x23, 2, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* stratum 0 carries a kiss code in the reference id */
            {0x24, 0, 0, 0, SNTP_KISS_RATE, true, SNTP_RATE_LIMITED},
            {0x24, 0, 0, 0, SNTP_KISS_DENY, true, SNTP_BAD_SERVER},
            {0x24, 0, 0, 0, SNTP_KISS_RSTR, true, SNTP_BAD_SERVER},
            {0x24, 0, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* RATE is still RATE when the alarm bit is set with it */
            {0xe4, 0, 0, 0, SNTP_KISS_RATE, true, SNTP_RATE_LIMITED},
            /* stratum 16 is unsynchronised, and the alarm says so too */
            {0x24, 16, 0, 0, 0, true, SNTP_BAD_SERVER},
            {0xe4, 2, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* a second of root delay is the most we trust, and the sign
               bit of the fixed-point short is never legitimately set */
            {0x24, 2, SNTP_SHORT_SECOND, SNTP_SHORT_SECOND, 0, true, SNTP_OK},
            {0x24, 2, SNTP_SHORT_SECOND + 1, 0, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0, SNTP_SHORT_SECOND + 1, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0x80000000u, 0, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0, 0x80000000u, 0, true, SNTP_BAD_SERVER},
        };
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

        memory_fill(request, 0, sizeof(request));
        request[0] = SNTP_LI_VN_MODE;
        network_store_32(request + 40, 0xc0ffee00u);
        network_store_32(request + 44, 0x0badf00du);
        for (at = 0; at < array_count(reply_case); at++)
        {
                memory_fill(reply, 0, sizeof(reply));
                reply[0] = reply_case[at].first;
                reply[1] = reply_case[at].stratum;
                network_store_32(reply + 4, reply_case[at].root_delay);
                network_store_32(reply + 8, reply_case[at].root_dispersion);
                network_store_32(reply + 12, reply_case[at].reference_id);
                if (reply_case[at].echoed)
                        memory_copy(reply + 24, request + 40, 8);
                if (sntp_reply_ok(reply, request) != reply_case[at].want)
                        return false;
        }

        for (at = 0; at < array_count(control_case); at++)
        {
                memory_zero(control, sizeof(control));
                address_to(positive address_to)control = control_case[at].claimed;
                address_to(b32 address_to)(control + sizeof(positive)) =
                    control_case[at].level;
                address_to(b32 address_to)(control + sizeof(positive) + 4) =
                    control_case[at].kind;
                address_to(p64 address_to)(control + SNTP_CONTROL_DATA) =
                    1700000000ull;
                address_to(p64 address_to)(control + SNTP_CONTROL_DATA +
                                           sizeof(p64)) = 250000000ull;
                arrived[0] = 0;
                arrived[1] = 0;
                if (sntp_control_stamp(control, control_case[at].held,
                                       SNTP_TIMESTAMPNS,
                                       arrived) != control_case[at].want)
                        return false;
                if (control_case[at].want &&
                    (arrived[0] != 1700000000ull || arrived[1] != 250000000ull))
                        return false;
        }

        /*
                The kernel puts its messages in the order it likes, so the
                one we want is not always first. A message of another kind
                in front of it must be stepped over, not stopped at.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPNS + 7;
        address_to(positive address_to)(control + SNTP_CONTROL_DATA) =
            SNTP_CONTROL_DATA + 16;
        address_to(b32 address_to)(control + SNTP_CONTROL_DATA +
                                   sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + SNTP_CONTROL_DATA +
                                   sizeof(positive) + 4) = SNTP_TIMESTAMPNS;
        address_to(p64 address_to)(control + 2 * SNTP_CONTROL_DATA) =
            1700000001ull;
        address_to(p64 address_to)(control + 2 * SNTP_CONTROL_DATA +
                                   sizeof(p64)) = 750000000ull;
        arrived[0] = 0;
        arrived[1] = 0;
        if (!sntp_control_stamp(control, 2 * SNTP_CONTROL_DATA + 16,
                                SNTP_TIMESTAMPNS, arrived) ||
            arrived[0] != 1700000001ull || arrived[1] != 750000000ull)
                return false;

        /*
                The departure stamp comes back under a different type and
                in a longer payload -- three timespecs, of which the
                software one is first -- so the walk has to take its two
                words from the front and let the rest alone, and has to
                tell the two types apart rather than taking whichever
                timestamp it meets first.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 48;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPING;
        address_to(p64 address_to)(control + SNTP_CONTROL_DATA) = 1700000002ull;
        address_to(p64 address_to)(control + SNTP_CONTROL_DATA + sizeof(p64)) =
            125000000ull;
        arrived[0] = 0;
        arrived[1] = 0;
        if (!sntp_control_stamp(control, SNTP_CONTROL_DATA + 48,
                                SNTP_TIMESTAMPING, arrived) ||
            arrived[0] != 1700000002ull || arrived[1] != 125000000ull)
                return false;
        if (sntp_control_stamp(control, SNTP_CONTROL_DATA + 48,
                               SNTP_TIMESTAMPNS, arrived))
                return false;

        /*
                The transmit stamp's number rides in the error header
                beside it, not in the stamp, and the same header says
                whether the entry is a timestamp at all. A real ICMP
                error carries a different origin and must not be read as
                a sequence number, or a refused port would start
                claiming to be the answer to a send.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA +
                                                 SNTP_ERROR_BYTES;
        address_to(b32 address_to)(control + sizeof(positive)) = SNTP_SOL_IP;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_IP_RECVERR;
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] =
            SNTP_ERROR_TIMESTAMPING;
        address_to(p32 address_to)(control + SNTP_CONTROL_DATA +
                                   SNTP_ERROR_SEQUENCE) = 4u;
        sequence = 0;
        if (!sntp_control_sequence(control, SNTP_CONTROL_DATA +
                                                SNTP_ERROR_BYTES,
                                   address_of sequence) ||
            sequence != 4u)
                return false;

        /* the same entry as an ICMP error rather than a timestamp */
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] = 2; /* ICMP */
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA +
                                               SNTP_ERROR_BYTES,
                                  address_of sequence))
                return false;

        /* a header cut short of the field the number sits in */
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] =
            SNTP_ERROR_TIMESTAMPING;
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 8;
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA + 8,
                                  address_of sequence))
                return false;

        /* and no error header at all, which is the fallback case */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 48;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPING;
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA + 48,
                                  address_of sequence))
                return false;

        /*
                One bit of the echoed stamp flipped is still a forgery.
        */
        memory_fill(reply, 0, sizeof(reply));
        reply[0] = 0x24;
        reply[1] = 2;
        memory_copy(reply + 24, request + 40, 8);
        reply[31] ^= 1;
        if (sntp_reply_ok(reply, request) != SNTP_NO_REPLY)
                return false;

        return sntp_pick(row, 5) == 2 && row[2].offset_ns == 2000000;
}

static HOT bipolar sntp_exchange(b32 handle,
                                 network_deadline address_to deadline,
                                 bool tight, p32 address_to sequence,
                                 sntp_sample address_to into)
{
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        p64 sent[2];
        p64 got[2];
        p64 spare[2];
        p32 mine;
        bipolar t1;
        bipolar t2;
        bipolar t3;
        bipolar t4;
        bipolar wait;
        bipolar received;
        bipolar verdict;
        bipolar reference;
        bool stamped;
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
        mine = address_to sequence;
        address_to sequence = mine + 1;
        (void)sntp_transmit_stamp(handle, mine, sent);
        t1 = sntp_timespec_ns(sent[0], sent[1]);
        if_rare (!sntp_local_ok(t1))
                return SNTP_NO_REPLY;

        for (;;)
        {
                wait = network_wait_readable_until(handle, deadline);
                if_rare (wait <= 0)
                        return SNTP_NO_REPLY;
                received = sntp_receive_stamped(handle, reply,
                                                sizeof(reply), got,
                                                address_of stamped);
                if_rare (!stamped &&
                         system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                                       (positive)got) < 0)
                        return SNTP_NO_REPLY;
                if_rare (received < 0)
                {
                        /*
                                Nothing was readable, so what woke the
                                wait was the error queue: a transmit
                                stamp that was not yet there when the
                                send drained for it. Take it off now --
                                t1 is already decided, so the stamp is
                                of no further use -- because a socket
                                with anything on that queue reports
                                POLLERR, and leaving it would wake this
                                wait again immediately, and again, until
                                the deadline ran out.
                        */
                        (void)sntp_transmit_stamp(handle, mine, spare);
                        continue;
                }
                if_rare (received < SNTP_PACKET)
                        continue;
                verdict = sntp_reply_ok(reply, request);
                if_rare (verdict == SNTP_NO_REPLY)
                        continue;
                if_rare (verdict < 0)
                        return verdict;
                t4 = sntp_timespec_ns(got[0], got[1]);
                if_rare (!sntp_local_ok(t4))
                        return SNTP_MALFORMED;
                t2 = sntp_load_stamp(reply + 32);
                t3 = sntp_load_stamp(reply + 40);
                /*
                        The reference stamp is when the server last set
                        its own clock, so it sits at or before the stamp
                        it transmits. A second of slack, because the two
                        are read at different moments and a server whose
                        reference is one tick the wrong side of transmit
                        would otherwise be refused for ever: the sample
                        loop stops on BAD_SERVER, so that server is not
                        asked again.
                */
                reference = sntp_load_stamp(reply + 16);
                if_rare (!sntp_wall_ok(reference) ||
                         reference > t3 + (bipolar)SNTP_NANOSECONDS)
                        return SNTP_BAD_SERVER;
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
        b32 want_stamp = 1;
        p32 want_transmit = SNTP_TIMESTAMPING_WANT;
        p32 sequence = 0;
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
        (void)socket_option_set((b32)handle, SOL_SOCKET, SNTP_TIMESTAMPNS,
                                address_of want_stamp, sizeof(want_stamp));
        (void)socket_option_set((b32)handle, SOL_SOCKET, SNTP_TIMESTAMPING,
                                address_of want_transmit,
                                sizeof(want_transmit));

        memory_zero(row, sizeof(row));
        for (at = 0; at < want; at++)
        {
                failed = sntp_exchange((b32)handle, address_of deadline, tight,
                                       address_of sequence, row + at);
                if (failed == SNTP_BAD_SERVER || failed == SNTP_RATE_LIMITED)
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


#define LOCALE_ZONE_PATH "/root/timezone"
#define LOCALE_NTP_PATH "/root/ntp"
#define LOCALE_NTP_SERVER_PATH "/root/ntp.server"
#define LOCALE_NTP_FILTER_PATH "/root/ntp.filter"
#define LOCALE_KEYBOARD_PATH "/root/keyboard"
#define LOCALE_NTP_DEFAULT_SERVER "pool.ntp.org"
#define LOCALE_NTP_RETRY_LEAST 1
#define LOCALE_NTP_RETRY_MOST 8
#define LOCALE_NTP_AGAIN 1800
#define LOCALE_WAIT_NOHANG 1
#define LOCALE_NTP_RATE_AGAIN 300
#define LOCALE_NTP_EXIT_RATE 2
#define LOCALE_NTP_STEP_NS ((bipolar)128 * 1000000)
#define LOCALE_NTP_TIMECONST 6
#define LOCALE_TIMEX_OFFSET 1
#define LOCALE_TIMEX_CONSTANT 6
/*
        The kernel's own numbering, from uapi/linux/timex.h. ADJ_SETOFFSET
        is 0x0100. It was 0x80 here, which is ADJ_TAI: a request to set the
        TAI offset from the constant word, not to step the clock from the
        time words. The kernel read word 6, which is zero, wrote that as
        the system TAI offset, ignored the offset we had gone to such
        lengths to measure, and returned the clock state -- a number the
        caller reads as success. So every sample, every filter and every
        guard below fed a call that could not set the clock, said it
        had, and cleared STA_UNSYNC on the way out so the machine reported
        itself synchronised.
*/
#define ADJ_OFFSET 0x0001
#define ADJ_FREQUENCY 0x0002
#define ADJ_MAXERROR 0x0004
#define ADJ_ESTERROR 0x0008
#define ADJ_STATUS 0x0010
#define ADJ_TIMECONST 0x0020
#define ADJ_SETOFFSET 0x0100
#define ADJ_NANO 0x2000
#define STA_PLL 0x0001
#define STA_UNSYNC 0x0040
#define STA_NANO 0x2000

static p64 locale_ntp_next;
static positive locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
static bipolar locale_ntp_child;

static fn locale_ntp_keep(void);

static fn locale_word(string_address path, p8 address_to into, positive room)
{
        bipolar got = host_read_text(path, into, room);
        positive length;

        if (got < 0)
        {
                into[0] = end;
                return;
        }
        length = string_length(into);
        while (length && (into[length - 1] == '\n' || into[length - 1] == '\r'))
                into[--length] = end;
}

static bool locale_switch_on(string_address path)
{
        p8 word[16];

        locale_word(path, word, sizeof(word));
        if (!word[0])
                return true;
        return string_equals(word, "on");
}

static bool locale_ntp_wanted(void)
{
        return locale_switch_on(LOCALE_NTP_PATH);
}

static bool locale_ntp_filter_wanted(void)
{
        return locale_switch_on(LOCALE_NTP_FILTER_PATH);
}

static bool locale_clock_synced(void)
{
        return logger_clock_synced(null);
}

static bool locale_zone_ok(string_address name)
{
        positive at;

        if (!name || !name[0])
                return false;
        if (clock_zone_posix(name))
                return true;
        for (at = 0; name[at]; at++)
                if (name[at] >= '0' && name[at] <= '9')
                        return true;
        return false;
}

static b32 locale_zone_status(void)
{
        p8 zone[80];
        p64 now[2] = {0, 0};
        tm broken;
        time_t stamp;
        p8 when[40];

        locale_word(LOCALE_ZONE_PATH, zone, sizeof(zone));
        if (!zone[0])
                string_copy_bounded(zone, "UTC", sizeof(zone));
        system_call_2(syscall(clock_gettime), CLOCK_REALTIME, (positive)now);
        stamp = (time_t)now[0];
        tzset();
        if (!localtime_r(address_of stamp, address_of broken))
                return host_fail("timezone", -1);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", address_of broken);
        string_format(log, host_label "timezone %s\n", zone);
        string_format(log, "  local %s %s\n", when,
                      (string_address)broken.tm_zone);
        log_flush();
        return 0;
}

static b32 locale_zone_set(string_address name)
{
        if (!locale_zone_ok(name) || !radio_text_plain(name, string_length(name)))
                return host_refuse("unknown timezone %s\n", name);
        if (radio_write_word(LOCALE_ZONE_PATH, name) < 0)
                return host_fail("timezone", -1);
        tzset();
        string_format(log, host_label "timezone %s\n", name);
        log_flush();
        return 0;
}

static fn locale_clock_mark_synced(void)
{
        positive words[LOGGER_TIMEX_WORDS] = {0};

        words[0] = ADJ_STATUS;
        words[LOGGER_TIMEX_STATUS] = STA_PLL;
        system_call_1(syscall(adjtimex), (positive)words);
}

/*
        A correction applied once and then left alone is only right at
        the moment it lands. What carries the clock between polls is a
        crystal, and LOCALE_NTP_AGAIN is half an hour: ten parts per
        million, which is an ordinary one, is eighteen milliseconds of
        drift by the next query. That is three orders of magnitude past
        every other error on this path put together, and no amount of
        care measuring the offset touches any of it. Stepping and then
        free-running for 1800 seconds spends the whole measurement in
        the first instant and then throws it away.

        The kernel keeps a phase-locked loop for exactly this, and it
        keeps its frequency estimate across our polls -- which is what
        this program needs, because the query runs in a forked child
        that exits, so nothing held in memory survives to the next one.
        Handing the offset to that loop with ADJ_OFFSET and STA_PLL lets
        the kernel both steer the clock and learn how fast it runs; a
        poll interval longer than MINSEC puts it in the frequency-locked
        regime, which is the one that estimates rate from samples as far
        apart as ours.

        A step is still right when the clock is far out. Slewing never
        moves time backwards, which is what a log, a build and a file
        timestamp all want, but the kernel slews at a bounded rate, so a
        large offset would take longer to walk off than the gap between
        polls. The split is at 128 ms, where ntpd puts it.

        A step also cancels any slew still in progress, with an
        ADJ_OFFSET of zero in the same request: the pending phase
        adjustment was computed against a clock this request is about to
        move, and applying both would correct twice.
*/
static CONST bool locale_ntp_wants_step(bipolar offset_ns)
{
        return offset_ns >= LOCALE_NTP_STEP_NS ||
               offset_ns <= -LOCALE_NTP_STEP_NS;
}

static fn locale_ntp_discipline_words(bipolar offset_ns, bipolar seconds,
                                      bipolar nanoseconds,
                                      positive address_to words)
{
        memory_zero(words, LOGGER_TIMEX_WORDS * sizeof(positive));
        words[LOGGER_TIMEX_STATUS] = STA_PLL;
        if (locale_ntp_wants_step(offset_ns))
        {
                words[0] = ADJ_SETOFFSET | ADJ_OFFSET | ADJ_NANO | ADJ_STATUS;
                words[LOCALE_TIMEX_OFFSET] = 0;
                words[LOGGER_TIMEX_TIME_SEC] = (positive)seconds;
                words[LOGGER_TIMEX_TIME_NSEC] = (positive)nanoseconds;
                return;
        }
        words[0] = ADJ_OFFSET | ADJ_TIMECONST | ADJ_NANO | ADJ_STATUS;
        words[LOCALE_TIMEX_OFFSET] = (positive)offset_ns;
        words[LOCALE_TIMEX_CONSTANT] = LOCALE_NTP_TIMECONST;
}

/*
        Nothing unprivileged can ask the kernel which mode a bit means,
        and a wrong one returns success, so the decision is checked here
        instead: what goes in the request for a given offset, rather than
        what the kernel does with it.
*/
static COLD bool locale_discipline_ok(void)
{
        positive words[LOGGER_TIMEX_WORDS];
        positive at;
        static const struct
        {
                bipolar offset_ns;
                bool step;
        } discipline_case[] = {
            {0, false},
            {1000000, false},
            {-1000000, false},
            {LOCALE_NTP_STEP_NS - 1, false},
            {-(LOCALE_NTP_STEP_NS - 1), false},
            {LOCALE_NTP_STEP_NS, true},
            {-LOCALE_NTP_STEP_NS, true},
            {(bipolar)86400 * 1000000000, true},
            {-(bipolar)86400 * 1000000000, true},
        };

        for (at = 0; at < array_count(discipline_case); at++)
        {
                bipolar offset = discipline_case[at].offset_ns;
                bipolar sec = 0;
                bipolar nsec = 0;

                sntp_split_offset(offset, address_of sec, address_of nsec);
                locale_ntp_discipline_words(offset, sec, nsec, words);

                if (locale_ntp_wants_step(offset) != discipline_case[at].step)
                        return false;
                /* the loop is enabled either way, and the clock counts as
                   set either way, so STA_UNSYNC never survives a reply */
                if (words[LOGGER_TIMEX_STATUS] != STA_PLL)
                        return false;
                if (words[0] & ADJ_STATUS ? false : true)
                        return false;
                if (discipline_case[at].step)
                {
                        /* a step carries the time, cancels any slew, and
                           has no business setting a loop time constant */
                        if (!(words[0] & ADJ_SETOFFSET) ||
                            words[0] & ADJ_TIMECONST ||
                            words[LOCALE_TIMEX_OFFSET] != 0 ||
                            (bipolar)words[LOGGER_TIMEX_TIME_SEC] != sec ||
                            (bipolar)words[LOGGER_TIMEX_TIME_NSEC] != nsec)
                                return false;
                }
                else
                {
                        /* a slew hands the offset to the loop and never
                           steps, so time does not go backwards */
                        if (words[0] & ADJ_SETOFFSET ||
                            !(words[0] & ADJ_TIMECONST) ||
                            (bipolar)words[LOCALE_TIMEX_OFFSET] != offset ||
                            words[LOCALE_TIMEX_CONSTANT] !=
                                LOCALE_NTP_TIMECONST ||
                            words[LOGGER_TIMEX_TIME_SEC] ||
                            words[LOGGER_TIMEX_TIME_NSEC])
                                return false;
                }
                if (!(words[0] & ADJ_NANO) || !(words[0] & ADJ_OFFSET))
                        return false;
        }
        return true;
}

static const char locale_ntp_fallback[][24] = {
        LOCALE_NTP_DEFAULT_SERVER,
        "time.google.com",
        "time.cloudflare.com",
        "216.239.35.0",
        "216.239.35.4",
        "162.159.200.1",
        "162.159.200.123",
};

static bipolar locale_ntp_apply_offset(bipolar offset_ns)
{
        bipolar now;
        bipolar target = 0;
        bipolar sec = 0;
        bipolar nsec = 0;
        positive words[LOGGER_TIMEX_WORDS];
        p64 stamp[2];
        bipolar failed;

        now = sntp_now_ns();
        if (now < 0)
                return now;
        if (!sntp_target_ok(now, offset_ns, address_of target))
                return SNTP_MALFORMED;

        sntp_split_offset(offset_ns, address_of sec, address_of nsec);
        locale_ntp_discipline_words(offset_ns, sec, nsec, words);
        failed = system_call_1(syscall(adjtimex), (positive)words);
        if_common (failed >= 0)
                return 0;

        now = sntp_now_ns();
        if (now < 0)
                return now;
        if (!sntp_target_ok(now, offset_ns, address_of target))
                return SNTP_MALFORMED;
        stamp[0] = (p64)(target / (bipolar)SNTP_NANOSECONDS);
        stamp[1] = (p64)(target % (bipolar)SNTP_NANOSECONDS);
        failed = system_call_2(syscall(clock_settime), CLOCK_REALTIME,
                               (positive)stamp);
        if (failed < 0)
        {
                stamp[1] = stamp[1] / 1000;
                failed = system_call_2(syscall(settimeofday), (positive)stamp, 0);
        }
        if (failed < 0)
                return failed;
        locale_clock_mark_synced();
        return 0;
}

static bipolar locale_ntp_one(string_address server, bool filter, bool tight)
{
        bipolar offset_ns = 0;
        bipolar failed;

        if (!server || !server[0] ||
            !radio_text_plain(server, string_length(server)))
                return SNTP_NO_SERVER;
        failed = sntp_query(server, filter, tight, address_of offset_ns);
        if (failed < 0)
                return failed;
        return locale_ntp_apply_offset(offset_ns);
}

/*
        A server answering RATE is telling us we ask too often. Walking to
        the next name and asking that one immediately is not an answer to
        it, and when the next name is another address of the same pool it
        is the complaint repeated. The verdict is carried out of the walk
        so a cycle that ended in nothing but rate limits waits properly
        instead of coming back in a second and doing it again.
*/
static bipolar locale_ntp_apply(void)
{
        p8 server[80];
        bipolar failed = SNTP_NO_SERVER;
        positive at;
        bool filter = locale_ntp_filter_wanted();
        bool tight = locale_clock_synced();
        bool rated = false;

        locale_word(LOCALE_NTP_SERVER_PATH, server, sizeof(server));
        if (server[0])
        {
                failed = locale_ntp_one((string_address)server, filter, tight);
                if (failed >= 0)
                        return 0;
                rated = failed == SNTP_RATE_LIMITED;
        }
        for (at = 0; at < array_count(locale_ntp_fallback); at++)
        {
                if (server[0] &&
                    string_equals((string_address)server,
                                  (string_address)locale_ntp_fallback[at]))
                        continue;
                failed = locale_ntp_one((string_address)locale_ntp_fallback[at],
                                        filter, tight);
                if (failed >= 0)
                        return 0;
                if (failed == SNTP_RATE_LIMITED)
                        rated = true;
        }
        return rated ? SNTP_RATE_LIMITED : failed;
}

static b32 locale_ntp_status(void)
{
        p8 server[80];
        bool wanted = locale_ntp_wanted();
        bool synced = locale_clock_synced();

        locale_word(LOCALE_NTP_SERVER_PATH, server, sizeof(server));
        if (!server[0])
                string_copy_bounded(server, LOCALE_NTP_DEFAULT_SERVER,
                                    sizeof(server));
        string_format(log, host_label "ntp %s, %s, filter %s, %s\n",
                      wanted ? "on" : "off", server,
                      locale_ntp_filter_wanted() ? "on" : "off",
                      synced ? "synchronised" : "waiting");
        log_flush();
        return 0;
}

static b32 locale_ntp_filter_status(void)
{
        string_format(log, host_label "ntp filter %s\n",
                      locale_ntp_filter_wanted() ? "on" : "off");
        log_flush();
        return 0;
}

static COLD b32 locale_ntp_filter_set(string_address word)
{
        if (!string_equals(word, "on") && !string_equals(word, "off"))
                return host_usage();
        if (radio_write_word(LOCALE_NTP_FILTER_PATH, word) < 0)
                return host_fail("ntp", -1);
        string_format(log, host_label "ntp filter %s\n", word);
        log_flush();
        return 0;
}

static b32 locale_ntp_set(string_address word)
{
        if (!string_equals(word, "on") && !string_equals(word, "off"))
                return host_usage();
        if (radio_write_word(LOCALE_NTP_PATH, word) < 0)
                return host_fail("ntp", -1);
        if (string_equals(word, "on"))
        {
                locale_ntp_next = 0;
                if (locale_ntp_apply() < 0)
                {
                        string_format(log, host_label "ntp on, waiting for a reply\n");
                        log_flush();
                        return 0;
                }
        }
        string_format(log, host_label "ntp %s\n", word);
        log_flush();
        return 0;
}

static const struct
{
        char name[8];
} locale_keyboards[] = {
        {"us"}, {"uk"}, {"gb"}, {"de"}, {"se"}, {"sv"}, {"no"}, {"nb"},
        {"dk"}, {"fi"}, {"fr"}, {"es"}, {"it"},
};

static bool locale_keyboard_ok(string_address name)
{
        positive at;

        for (at = 0; at < array_count(locale_keyboards); at++)
                if (string_equals(name, (string_address)locale_keyboards[at].name))
                        return true;
        return false;
}

static b32 locale_keyboard_live(string_address name)
{
        struct canvas_control control;

        memory_zero(address_of control, sizeof(control));
        control.request = SPARK_CANVAS_LAYOUT;
        if (name)
        {
                positive length = string_length(name);

                if (length >= sizeof(control.master_command))
                        return -22;
                memory_copy(control.master_command, name, length);
        }
        return host_spark_once(SPARK_IOCTL_CANVAS, address_of control, FILE_READ);
}

static b32 locale_keyboard_status(void)
{
        p8 name[16];
        struct canvas_control control;

        locale_word(LOCALE_KEYBOARD_PATH, name, sizeof(name));
        if (!name[0])
                string_copy_bounded(name, "us", sizeof(name));
        memory_zero(address_of control, sizeof(control));
        control.request = SPARK_CANVAS_LAYOUT;
        if (host_spark_once(SPARK_IOCTL_CANVAS, address_of control, FILE_READ) >= 0 &&
            control.master_command[0])
                string_copy_bounded(name, control.master_command, sizeof(name));
        string_format(log, host_label "keyboard %s\n", name);
        log_flush();
        return 0;
}

static b32 locale_keyboard_set(string_address name)
{
        if (!locale_keyboard_ok(name))
                return host_refuse("unknown keyboard layout %s\n", name);
        if (radio_write_word(LOCALE_KEYBOARD_PATH, name) < 0)
                return host_fail("keyboard", -1);
        (void)locale_keyboard_live(name);
        string_format(log, host_label "keyboard %s\n", name);
        log_flush();
        return 0;
}

static fn locale_restore(void)
{
        p8 zone[80];
        p8 keyboard[16];

        locale_word(LOCALE_ZONE_PATH, zone, sizeof(zone));
        if (zone[0])
                tzset();

        locale_word(LOCALE_KEYBOARD_PATH, keyboard, sizeof(keyboard));
        if (keyboard[0])
                locale_keyboard_live(keyboard);

        locale_ntp_next = 0;
        locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
        locale_ntp_child = 0;
        if (locale_ntp_wanted())
                locale_ntp_keep();
}

static fn locale_ntp_keep(void)
{
        p64 now = system_clock_ns(HOST_CLOCK_BOOTTIME);
        bipolar child;

        if (locale_ntp_child > 0)
        {
                positive status = 0;
                bipolar reaped = system_wait4_retry(locale_ntp_child,
                                                    address_of status,
                                                    LOCALE_WAIT_NOHANG, null);

                if (reaped == 0)
                        return;
                locale_ntp_child = 0;
                if (locale_clock_synced())
                {
                        locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
                        locale_ntp_next =
                            now + (p64)LOCALE_NTP_AGAIN * 1000000000ull;
                }
                else if (((status >> 8) & 0xff) == LOCALE_NTP_EXIT_RATE)
                {
                        locale_ntp_retry = LOCALE_NTP_RETRY_MOST;
                        locale_ntp_next =
                            now + (p64)LOCALE_NTP_RATE_AGAIN * 1000000000ull;
                }
                else
                {
                        locale_ntp_next =
                            now + (p64)locale_ntp_retry * 1000000000ull;
                        if (locale_ntp_retry < LOCALE_NTP_RETRY_MOST)
                                locale_ntp_retry *= 2;
                }
                return;
        }

        if (locale_ntp_next && now < locale_ntp_next)
                return;

        child = system_fork();
        if (child < 0)
                return;
        if (!child)
        {
                bipolar failed = locale_ntp_apply();

                system_call_1(syscall(exit),
                              failed >= 0
                                  ? 0
                                  : (failed == SNTP_RATE_LIMITED
                                         ? LOCALE_NTP_EXIT_RATE
                                         : 1));
        }
        locale_ntp_child = child;
}

static fn locale_recover(void)
{
        if (locale_ntp_wanted())
                locale_ntp_keep();
}

static b32 host_locale(string_address address_to arguments, positive count)
{
        string_address verb = arguments[1];
        string_address word = count > 2 ? arguments[2] : null;

        if (string_equals(verb, "timezone"))
        {
                if (count == 2)
                        return locale_zone_status();
                if (count != 3)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return locale_zone_set(word);
        }

        if (string_equals(verb, "ntp"))
        {
                if (count == 2)
                        return locale_ntp_status();
                if (string_equals(word, "filter"))
                {
                        if (count == 3)
                                return locale_ntp_filter_status();
                        if (count != 4)
                                return host_usage();
                        if (!bowl_is_root())
                                return host_refuse("%s needs root\n", "moonwater");
                        return locale_ntp_filter_set(arguments[3]);
                }
                if (count != 3)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return locale_ntp_set(word);
        }

        if (count == 2)
                return locale_keyboard_status();
        if (count != 3)
                return host_usage();
        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");
        return locale_keyboard_set(word);
}
