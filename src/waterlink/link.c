/*
        Waterlink's core, as a transform.

        Frames in, datagram bodies out, and the same in reverse. Nothing here
        opens a socket, reads a clock, takes memory or touches a key: the
        caller passes the time it already read and the buffer it already owns,
        and gets bytes back. That is not tidiness. It is what lets the same
        code be the kernel's datapath on Moonwater and a userspace socket loop
        on a machine that is not Moonwater, and it is what lets the scheduler,
        the retransmission and the replay window be tested to the end without
        a network.

        The rules this file implements are stated in waterlink.c and are not
        restated here. What is here is how they are kept.

        Time is microseconds, from whatever clock the caller keeps, as long as
        it never goes backwards. Deadlines on the wire stay milliseconds.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_LINK_INCLUDED
#define WATERLINK_LINK_INCLUDED

#include "waterlink.c"

// The largest frame that can share a datagram with nothing else.
#define WATERLINK_FRAME_MAX (WATERLINK_PAYLOAD - WATERLINK_HEADER)

/*
        Ceilings, not guesses at a working set. A fixed count is what buys no
        allocation failure on the datapath and a cost that does not depend on
        what the far side does. If one is ever too low, raise it here -- that
        is the whole intent of naming it.

        SLOTS is what may be queued or unacknowledged at once, so it is also
        the most a link can have in flight: 256 frames of 1140 bytes is 285 KB
        a round trip, 14 MB/s at 20 ms and far past a gigabyte a second over a
        loopback's tens of microseconds. HELD is what the receiver keeps of
        frames that arrived before the frame they follow; a key may have no
        more than KEY_WINDOW frames in flight, so one lost frame on a key
        never strands more behind it than the pool can hold.
*/
#define WATERLINK_SLOTS 256 // frames queued or awaiting acknowledgement
#define WATERLINK_KEYS 512  // live keys tracked in each direction
#define WATERLINK_HELD 64   // frames held back for the frame they follow
#define WATERLINK_KEY_WINDOW 64
#define WATERLINK_ACKS 128 // keys with an acknowledgement owed

#define WATERLINK_NONE 0xffffffffu

/*
        Three queues, because urgency is a band and not a sort.

        Sorting by deadline would be the obvious thing and is the wrong thing:
        it puts a scan on the send path, and within a band post order is
        already the order a receiver will want. So a band is a list, a frame
        joins the end of its band's list, and supersession replaces a frame
        where it stands rather than moving it. A frame that was lost goes back
        to the front of its band, since the frames behind it on its key are
        waiting for it at the far side.
*/
#define WATERLINK_BAND_URGENT 0
#define WATERLINK_BAND_TIMED 1
#define WATERLINK_BAND_BULK 2
#define WATERLINK_BANDS 3

#define WATERLINK_SLOT_FREE 0
#define WATERLINK_SLOT_QUEUED 1 // in its band, waiting to be sent
#define WATERLINK_SLOT_FLIGHT 2 // sent, waiting for its acknowledgement
#define WATERLINK_SLOT_HELD 3   // arrived, waiting for the frame it follows

struct waterlink_slot {
        p64 key;
        p64 posted; // for the deadline
        p64 sent;   // the last transmission
        p64 serial; // the last transmission's place in the link's order
        p32 sequence;
        p32 follows;
        p32 next;  // the next slot in its band or in the flight
        p32 prior; // the one before it in the flight
        p32 chain; // the next slot on the same key, by sequence
        p16 channel;
        p16 length;
        p16 deadline;
        p16 flags;
        p16 inflated;
        p8 state;
        p8 tries;
        p8 required; // replaceable, and a durable frame follows it
        p8 unused;
        p8 payload[WATERLINK_FRAME_MAX];
};

/*
        One key, in one direction.

        Sending, it holds the next sequence to assign, the sequence the next
        frame must follow, and the chain of this key's slots from oldest to
        newest -- the newest is the only one supersession ever asks about.

        Receiving, it holds how far the key has been delivered, the highest
        sequence seen at all, and the frames held back for the one they
        follow. A key that ended stays here, remembered as ended, until the
        table needs the entry: that memory is what tells a late copy of the
        last frame from something new.
*/
struct waterlink_live {
        p64 key;
        p64 ended;     // receiving: when the last frame was delivered
        p32 sequence;  // sending: the next to assign; receiving: delivered
        p32 floor;     // sending: what the next frame follows; receiving: seen
        p32 first;     // sending: the oldest slot; receiving: the first held
        p32 last;      // sending: the newest slot
        p16 flying;    // sending: slots of this key in flight
        p8 taken;
        p8 closing;    // sending: the last frame is posted
        p8 acking;     // receiving: an acknowledgement is owed
        p8 over;       // receiving: the key has ended
        p8 unused[2];
};

/*
        A frame that arrived before the frame it follows, kept until that one
        comes. Chained per key by sequence.
*/
struct waterlink_held {
        struct waterlink_frame head;
        p32 next;
        p8 payload[WATERLINK_FRAME_MAX];
};

/*
        The replay window: the highest counter seen, and a bitmap of which of
        the WATERLINK_REPLAY_WINDOW counters below it have already arrived.
        A datagram above the top slides the window; one below it is checked
        and marked; one below the bottom is too old to judge and is refused.
*/
struct waterlink_replay {
        p64 top;
        p64 seen[WATERLINK_REPLAY_WINDOW / 64];
};

struct waterlink_link {
        struct waterlink_slot slot[WATERLINK_SLOTS];
        struct waterlink_held held[WATERLINK_HELD];
        struct waterlink_live sending[WATERLINK_KEYS];
        struct waterlink_live receiving[WATERLINK_KEYS];
        struct waterlink_replay replay;
        p64 acking[WATERLINK_ACKS];

        p32 head[WATERLINK_BANDS];
        p32 tail[WATERLINK_BANDS];
        p32 requeue[WATERLINK_BANDS]; // where the next lost frame goes back in
        p32 free;
        p32 held_free;
        p32 flight_head;
        p32 flight_tail;
        p32 acks;

        /*      How much the path carries, as far as this side can tell. The
                window and what is in flight are frame bytes; an estimate of
                the round trip in microseconds, zero until the first sample. */
        p64 clock;
        p64 serial;        // transmissions so far
        p64 largest;       // the latest transmission known to have arrived
        p64 recovery;      // transmissions before this were sent pre-loss
        p64 in_flight;
        p64 window;
        p64 threshold;
        p64 smoothed;
        p64 variance;
        p64 pace;          // when the next paced datagram may leave
        p64 recent;        // the last round trip sampled
        p64 owed;          // when the oldest unacknowledged arrival came
        p32 owed_count;    // datagrams with frames since the last ack
        p8 owed_now;       // an arrival that should be answered at once
        p8 carried;        // the last fill held a frame, not only acks
        p8 unused_flags[2];
        p32 backoff;
        p32 probes;        // datagrams the probe timer lets past the window

        // What the caller may want to know without instrumenting the caller.
        p64 posted;
        p64 superseded;
        p64 expired;
        p64 refused;
        p64 delivered;
        p64 stale;
        p64 sent;
        p64 retransmitted;
        p64 lost;
        p64 timeouts;
        p64 acked;
        p64 kept;    // frames held back
        p64 spilled; // frames dropped because the hold-back was full
};

/*
        How the path is estimated. Not part of the contract, which only says a
        sender must not put more in flight than the path carries: this is
        additive increase and halving on loss, a window that starts at ten
        datagrams and doubles per round trip until the first loss, and a
        pacer that spreads a window over a round trip at a quarter faster
        than the window, so a run of segments leaves as a run and not as the
        whole window at once. The retransmission timer is the usual smoothed
        round trip plus four deviations, doubled on each expiry.
*/
#define WATERLINK_DATAGRAM_BYTES WATERLINK_DATAGRAM
#define WATERLINK_WINDOW_FIRST (10 * WATERLINK_DATAGRAM)
#define WATERLINK_WINDOW_LEAST (2 * WATERLINK_DATAGRAM)
#define WATERLINK_WINDOW_MOST ((p64)WATERLINK_SLOTS * WATERLINK_DATAGRAM)
#define WATERLINK_REORDER 3             // transmissions after, then lost
#define WATERLINK_RTT_FIRST 100000ull   // before any sample
#define WATERLINK_GRANULE 1000ull       // the least a timer means
#define WATERLINK_RTO_MOST 2000000ull
#define WATERLINK_PERSISTENT 3          // probe expiries, then the path is gone

/*      Acknowledgements are owed every second datagram that carried frames,
        and never later than this: at once when a frame arrived out of
        order, was a copy, was urgent or ended its key, since each of those
        is something the sender is waiting to hear. Otherwise an
        acknowledgement rides free in whatever datagram goes the other way
        first. */
#define WATERLINK_ACK_EVERY 2
#define WATERLINK_ACK_DELAY 1000ull
#define WATERLINK_PACE_BURST 16         // datagrams the pacer lets go at once

/*
        Key tables are open addressed with linear probing. Deleting walks the
        run behind the hole back, which is the one deletion a linear probe
        table can do without tombstones.
*/
static p32 waterlink_key_home(p64 key)
{
        return (p32)((key * 0x9e3779b97f4a7c15ull) >> 55) &
               (WATERLINK_KEYS - 1);
}

static p32 waterlink_key_find(struct waterlink_live address_to table, p64 key)
{
        p32 at = waterlink_key_home(key);

        for (p32 step = 0; step < WATERLINK_KEYS; step++)
        {
                if (!table[at].taken)
                        return WATERLINK_NONE;
                if (table[at].key == key)
                        return at;
                at = (at + 1) & (WATERLINK_KEYS - 1);
        }

        return WATERLINK_NONE;
}

static p32 waterlink_key_make(struct waterlink_live address_to table, p64 key,
                              p32 start)
{
        p32 at = waterlink_key_home(key);

        for (p32 step = 0; step < WATERLINK_KEYS; step++)
        {
                struct waterlink_live address_to live = table + at;

                if (live->taken && live->key == key)
                        return at;

                if (!live->taken)
                {
                        memory_zero(live, sizeof(address_to live));
                        live->key = key;
                        live->sequence = start;
                        live->first = WATERLINK_NONE;
                        live->last = WATERLINK_NONE;
                        live->taken = 1;
                        return at;
                }

                at = (at + 1) & (WATERLINK_KEYS - 1);
        }

        return WATERLINK_NONE;
}

static fn waterlink_key_retire(struct waterlink_live address_to table, p32 at)
{
        p32 hole = at;
        p32 scan = (at + 1) & (WATERLINK_KEYS - 1);

        table[hole].taken = 0;

        //      Everything after a hole may have probed past it, so each entry
        //      in the run is asked where it wanted to be and moved back if
        //      the hole is on its way there.
        while (table[scan].taken)
        {
                p32 wanted = waterlink_key_home(table[scan].key);
                p32 span = (scan - wanted) & (WATERLINK_KEYS - 1);
                p32 reach = (scan - hole) & (WATERLINK_KEYS - 1);

                if (span >= reach)
                {
                        table[hole] = table[scan];
                        table[scan].taken = 0;
                        hole = scan;
                }

                scan = (scan + 1) & (WATERLINK_KEYS - 1);
        }
}

fn waterlink_link_reset(struct waterlink_link address_to link)
{
        memory_zero(link, sizeof(address_to link));

        for (p32 at = 0; at < WATERLINK_SLOTS; at++)
                link->slot[at].next = at + 1 < WATERLINK_SLOTS
                                              ? at + 1
                                              : WATERLINK_NONE;

        for (p32 at = 0; at < WATERLINK_HELD; at++)
                link->held[at].next = at + 1 < WATERLINK_HELD
                                              ? at + 1
                                              : WATERLINK_NONE;

        link->free = 0;
        link->held_free = 0;
        link->flight_head = WATERLINK_NONE;
        link->flight_tail = WATERLINK_NONE;

        for (p32 band = 0; band < WATERLINK_BANDS; band++)
        {
                link->head[band] = WATERLINK_NONE;
                link->tail[band] = WATERLINK_NONE;
                link->requeue[band] = WATERLINK_NONE;
        }

        link->window = WATERLINK_WINDOW_FIRST;
        link->threshold = ~0ull;
}

static p32 waterlink_band_of(p16 flags)
{
        if (flags & WATERLINK_FRAME_URGENT)
                return WATERLINK_BAND_URGENT;
        if (flags & WATERLINK_FRAME_BULK)
                return WATERLINK_BAND_BULK;
        return WATERLINK_BAND_TIMED;
}

static p64 waterlink_bytes(struct waterlink_slot address_to slot)
{
        return WATERLINK_HEADER + slot->length;
}

/*
        Whether a caller's frame is one this link will carry at all.

        Every one of these is a refusal and not a correction, because each
        names a way the far side could be made to unpack something the sender
        did not mean. A frame that is both replaceable and durable has no
        answer to "may this be dropped"; a replaceable frame carrying a coder's
        history makes every later frame on its key undecodable the moment it is
        dropped, which is the rule spelled out at WATERLINK_PACK_NONE; and an
        inflated length is a promise about somebody else's scratch buffer.
        The acknowledgement flag is the link's own and no caller may set it.
*/
static bool waterlink_frame_sane(p16 flags, p16 length, p16 inflated)
{
        bool replaceable = (flags & WATERLINK_FRAME_REPLACEABLE) != 0;
        bool durable = (flags & WATERLINK_FRAME_DURABLE) != 0;

        if (replaceable == durable)
                return false;

        if (flags & WATERLINK_FRAME_ACK)
                return false;

        if (length > WATERLINK_FRAME_MAX)
                return false;

        if ((flags & WATERLINK_FRAME_HISTORY) && replaceable)
                return false;

        //      No bound on inflated: the field is sixteen bits and so is
        //      WATERLINK_INFLATED, which is the point of choosing it.
        if (!inflated != !(flags & WATERLINK_FRAME_PACK_MASK))
                return false;

        return true;
}

// The flight is a list in the order things were sent.
static fn waterlink_flight_append(struct waterlink_link address_to link, p32 at)
{
        link->slot[at].next = WATERLINK_NONE;
        link->slot[at].prior = link->flight_tail;
        if (link->flight_tail == WATERLINK_NONE)
                link->flight_head = at;
        else
                link->slot[link->flight_tail].next = at;
        link->flight_tail = at;
}

static fn waterlink_flight_remove(struct waterlink_link address_to link,
                                  p32 at)
{
        struct waterlink_slot address_to slot = link->slot + at;
        p32 key_at = waterlink_key_find(link->sending, slot->key);

        if (slot->prior == WATERLINK_NONE)
                link->flight_head = slot->next;
        else
                link->slot[slot->prior].next = slot->next;

        if (slot->next == WATERLINK_NONE)
                link->flight_tail = slot->prior;
        else
                link->slot[slot->next].prior = slot->prior;

        link->in_flight -= waterlink_bytes(slot);
        if (key_at != WATERLINK_NONE && link->sending[key_at].flying)
                link->sending[key_at].flying--;
        slot->next = WATERLINK_NONE;
        slot->prior = WATERLINK_NONE;
}

static fn waterlink_band_append(struct waterlink_link address_to link,
                                p32 band, p32 at)
{
        link->slot[at].next = WATERLINK_NONE;
        if (link->tail[band] == WATERLINK_NONE)
                link->head[band] = at;
        else
                link->slot[link->tail[band]].next = at;
        link->tail[band] = at;
}

// Unlink one slot from its band, given the slot before it or NONE.
static fn waterlink_band_unlink(struct waterlink_link address_to link,
                                p32 band, p32 at, p32 prior)
{
        p32 next = link->slot[at].next;

        if (prior == WATERLINK_NONE)
                link->head[band] = next;
        else
                link->slot[prior].next = next;

        if (link->tail[band] == at)
                link->tail[band] = prior;
        if (link->requeue[band] == at)
                link->requeue[band] = prior;

        link->slot[at].next = WATERLINK_NONE;
}

static fn waterlink_band_remove(struct waterlink_link address_to link, p32 at)
{
        p32 band = waterlink_band_of(link->slot[at].flags);
        p32 prior = WATERLINK_NONE;

        for (p32 look = link->head[band]; look != WATERLINK_NONE;
             look = link->slot[look].next)
        {
                if (look == at)
                {
                        waterlink_band_unlink(link, band, at, prior);
                        return;
                }
                prior = look;
        }
}

/*
        A lost frame goes back ahead of everything posted since and behind
        every frame lost before it that has not gone out again yet: the front
        of a band is a queue of retransmissions in the order they were found.
        Putting the newest loss first instead starves the oldest -- which is
        the one the far side is holding everything else back for.
*/
static fn waterlink_band_requeue(struct waterlink_link address_to link, p32 at)
{
        p32 band = waterlink_band_of(link->slot[at].flags);
        p32 after = link->requeue[band];

        link->slot[at].state = WATERLINK_SLOT_QUEUED;

        if (after == WATERLINK_NONE)
        {
                link->slot[at].next = link->head[band];
                link->head[band] = at;
                if (link->tail[band] == WATERLINK_NONE)
                        link->tail[band] = at;
        }
        else
        {
                link->slot[at].next = link->slot[after].next;
                link->slot[after].next = at;
                if (link->tail[band] == after)
                        link->tail[band] = at;
        }

        link->requeue[band] = at;
}

/*
        A slot leaves its key's chain and goes back to the free list. The
        state has already been taken off the band or the flight. When the
        chain is empty and the key's last frame was posted, the key is gone
        from this side: it ended, and the far side said so.
*/
static fn waterlink_slot_free(struct waterlink_link address_to link, p32 at)
{
        struct waterlink_slot address_to slot = link->slot + at;
        p32 key_at = waterlink_key_find(link->sending, slot->key);

        if (key_at != WATERLINK_NONE)
        {
                struct waterlink_live address_to live = link->sending + key_at;
                p32 prior = WATERLINK_NONE;

                for (p32 look = live->first; look != WATERLINK_NONE;
                     look = link->slot[look].chain)
                {
                        if (look == at)
                        {
                                if (prior == WATERLINK_NONE)
                                        live->first = slot->chain;
                                else
                                        link->slot[prior].chain = slot->chain;
                                if (live->last == at)
                                        live->last = prior;
                                break;
                        }
                        prior = look;
                }

                if (live->first == WATERLINK_NONE && live->closing)
                        waterlink_key_retire(link->sending, key_at);
        }

        slot->state = WATERLINK_SLOT_FREE;
        slot->chain = WATERLINK_NONE;
        slot->next = link->free;
        link->free = at;
}

/*
        A replaceable frame whose deadline has passed is worthless -- unless a
        durable frame was posted behind it, which makes it a frame that one
        follows, and then a late truth still beats a hole the far side would
        wait on forever.
*/
static bool waterlink_slot_late(struct waterlink_slot address_to slot,
                                p64 now)
{
        return slot->deadline && !slot->required &&
               (slot->flags & WATERLINK_FRAME_REPLACEABLE) &&
               now - slot->posted > (p64)slot->deadline * 1000;
}

/*
        Queue a frame for sending.

        The supersession rule, in the one place it lives: a replaceable frame
        whose key's newest frame is replaceable takes that frame's slot. If
        the old value is still queued it is replaced where it stands; if it
        was sent and not yet acknowledged, the new value goes out in its
        place and the old transmission is abandoned -- the far side may apply
        it, or not, and either is right, because nothing durable sits between
        them. A durable frame always takes a new slot, and makes a replaceable
        frame in front of it one it follows.

        Returns false and counts a refusal when the frame is malformed, the
        queue is full, the key ran out of sequence numbers, or the key's last
        frame was already posted. A full queue is the caller's signal to stop
        producing, not this file's to start choosing.
*/
bool waterlink_post(struct waterlink_link address_to link, p64 key, p16 channel,
                    p16 flags, p16 deadline, p16 inflated,
                    address_any payload, p16 length, p64 now)
{
        struct waterlink_slot address_to slot;
        struct waterlink_live address_to live;
        p32 key_at;
        p32 band;
        p32 at;

        if (now > link->clock)
                link->clock = now;

        if (!waterlink_frame_sane(flags, length, inflated))
        {
                link->refused++;
                return false;
        }

        //      A new key is made only for a post a slot can carry: one made
        //      and then refused would stay in the table with nothing to
        //      retire it, and enough of them fill it for good.
        key_at = waterlink_key_find(link->sending, key);
        if (key_at == WATERLINK_NONE && link->free != WATERLINK_NONE)
                key_at = waterlink_key_make(link->sending, key, 1);
        if (key_at == WATERLINK_NONE)
        {
                link->refused++;
                return false;
        }

        live = link->sending + key_at;
        band = waterlink_band_of(flags);

        if (live->closing || live->sequence == WATERLINK_NONE)
        {
                link->refused++;
                return false;
        }

        if (live->last != WATERLINK_NONE)
        {
                slot = link->slot + live->last;

                if ((slot->flags & WATERLINK_FRAME_REPLACEABLE) &&
                    !slot->required)
                {
                        //      Supersession: same band, same slot.
                        if ((flags & WATERLINK_FRAME_REPLACEABLE) &&
                            waterlink_band_of(slot->flags) == band)
                        {
                                if (slot->state == WATERLINK_SLOT_FLIGHT)
                                        waterlink_flight_remove(link,
                                                                live->last);
                                if (slot->state != WATERLINK_SLOT_QUEUED)
                                {
                                        slot->state = WATERLINK_SLOT_QUEUED;
                                        waterlink_band_append(link, band,
                                                              live->last);
                                }

                                slot->sequence = live->sequence++;
                                slot->follows = live->floor;
                                slot->posted = now;
                                slot->sent = 0;
                                slot->tries = 0;
                                slot->channel = channel;
                                slot->length = length;
                                slot->deadline = deadline;
                                slot->flags = flags;
                                slot->inflated = inflated;
                                if (length)
                                        memory_copy(slot->payload, payload,
                                                    length);
                                link->superseded++;
                                return true;
                        }

                        //      A durable frame behind it: now it is followed.
                        if (flags & WATERLINK_FRAME_DURABLE)
                        {
                                slot->required = 1;
                                live->floor = slot->sequence;
                        }
                }
        }

        at = link->free;
        if (at == WATERLINK_NONE)
        {
                link->refused++;
                return false;
        }

        slot = link->slot + at;
        link->free = slot->next;

        slot->key = key;
        slot->sequence = live->sequence++;
        slot->follows = live->floor;
        slot->posted = now;
        slot->sent = 0;
        slot->serial = 0;
        slot->prior = WATERLINK_NONE;
        slot->chain = WATERLINK_NONE;
        slot->channel = channel;
        slot->length = length;
        slot->deadline = deadline;
        slot->flags = flags;
        slot->inflated = inflated;
        slot->state = WATERLINK_SLOT_QUEUED;
        slot->tries = 0;
        slot->required = 0;
        if (length)
                memory_copy(slot->payload, payload, length);

        if (flags & WATERLINK_FRAME_DURABLE)
                live->floor = slot->sequence;

        if (live->last == WATERLINK_NONE)
                live->first = at;
        else
                link->slot[live->last].chain = at;
        live->last = at;

        if (flags & WATERLINK_FRAME_LAST)
                live->closing = 1;

        waterlink_band_append(link, band, at);
        link->posted++;
        return true;
}

/*
        The probe timer, from the estimate: before any sample, the first
        guess; after, the smoothed round trip, four deviations (a millisecond
        at least) and the longest the far side may hold an acknowledgement,
        doubled for each expiry since the last acknowledgement. Leave the
        acknowledgement delay out and a sender that stops -- at the end of a
        burst, or waiting on the far side's credit -- times out on every
        last datagram the far side is still entitled to sit on: 223 expiries
        in a 100 MB stream over loopback, at 71 MB/s.
*/
static p64 waterlink_timeout(struct waterlink_link address_to link)
{
        p64 spread = 4 * link->variance;
        p64 base;
        p32 doubling = link->backoff < 10 ? link->backoff : 10;

        if (spread < WATERLINK_GRANULE)
                spread = WATERLINK_GRANULE;
        base = link->smoothed ? link->smoothed + spread + WATERLINK_ACK_DELAY
                              : 3 * WATERLINK_RTT_FIRST;
        base <<= doubling;
        return base < WATERLINK_RTO_MOST ? base : WATERLINK_RTO_MOST;
}

static fn waterlink_congested(struct waterlink_link address_to link)
{
        p64 half = link->window / 2;

        link->window = half > WATERLINK_WINDOW_LEAST ? half
                                                     : WATERLINK_WINDOW_LEAST;
        link->threshold = link->window;
        link->recovery = link->serial;
}

static fn waterlink_lose(struct waterlink_link address_to link, p32 at,
                         p64 now)
{
        waterlink_flight_remove(link, at);
        link->lost++;

        if (waterlink_slot_late(link->slot + at, now))
        {
                link->expired++;
                waterlink_slot_free(link, at);
        }
        else
                waterlink_band_requeue(link, at);
}

/*
        What is in flight and cannot have arrived: sent three transmissions
        or more before one that did, or sent before one that did and older
        than an eighth over the round trip. Lost frames go back to the front
        of their bands; a replaceable one whose deadline passed goes nowhere,
        since sending it again would be sending something worthless. One
        halving per round of loss, found by the transmission number: a loss
        of something sent before the last halving is the same congestion the
        halving already answered.

        When nothing sent later has arrived there is no evidence either way
        -- the last frames of a burst, or all of them -- and the probe timer
        answers that: it sends the oldest frame again, past the window, to
        make the far side say what it has. Only when it has expired three
        times with no answer is the path taken to be gone, everything in
        flight counted lost and the window brought to its least.
*/
static fn waterlink_losses(struct waterlink_link address_to link, p64 now)
{
        p64 trip = link->smoothed > link->recent ? link->smoothed
                                                 : link->recent;
        p64 threshold = trip + trip / 8;
        p32 at = link->flight_head;

        if (threshold < WATERLINK_GRANULE)
                threshold = WATERLINK_GRANULE;

        while (at != WATERLINK_NONE)
        {
                struct waterlink_slot address_to slot = link->slot + at;
                p32 next = slot->next;
                bool behind = slot->serial < link->largest;

                if (!behind)
                        break;

                if (slot->serial + WATERLINK_REORDER <= link->largest ||
                    now - slot->sent >= threshold)
                {
                        if (slot->serial > link->recovery)
                                waterlink_congested(link);
                        waterlink_lose(link, at, now);
                }

                at = next;
        }

        at = link->flight_head;
        if (at == WATERLINK_NONE ||
            now - link->slot[at].sent < waterlink_timeout(link))
                return;

        link->timeouts++;
        if (link->backoff < 16)
                link->backoff++;

        if (link->backoff >= WATERLINK_PERSISTENT)
        {
                while (link->flight_head != WATERLINK_NONE)
                        waterlink_lose(link, link->flight_head, now);
                link->threshold = link->window / 2 > WATERLINK_WINDOW_LEAST
                                          ? link->window / 2
                                          : WATERLINK_WINDOW_LEAST;
                link->window = WATERLINK_WINDOW_LEAST;
                link->recovery = link->serial;
        }
        else
                waterlink_lose(link, at, now);

        link->probes = 2;
}

/*
        A key may run no more than its window ahead of its oldest frame not
        yet acknowledged -- counting what the far side holds back as well as
        what is on the path, since held frames are what fill its pool.
*/
static bool waterlink_key_blocked(struct waterlink_link address_to link,
                                  struct waterlink_slot address_to slot)
{
        p32 key_at = waterlink_key_find(link->sending, slot->key);
        p32 first;

        if (key_at == WATERLINK_NONE)
                return false;

        first = link->sending[key_at].first;
        return first != WATERLINK_NONE &&
               slot->sequence - link->slot[first].sequence >=
                       WATERLINK_KEY_WINDOW;
}

/*
        Take the next frame worth sending from a band, dropping what has run
        out of time on the way and passing over a key that already has a full
        window in flight. The slot before the one returned comes back too,
        so the caller can unlink it without walking again.
*/
static p32 waterlink_band_take(struct waterlink_link address_to link, p32 band,
                               p64 now, p32 address_to prior_out)
{
        p32 prior = WATERLINK_NONE;
        p32 at = link->head[band];

        while (at != WATERLINK_NONE)
        {
                struct waterlink_slot address_to slot = link->slot + at;
                p32 next = slot->next;

                if (waterlink_slot_late(slot, now))
                {
                        waterlink_band_unlink(link, band, at, prior);
                        waterlink_slot_free(link, at);
                        link->expired++;
                        at = next;
                        continue;
                }

                if (band != WATERLINK_BAND_URGENT &&
                    waterlink_key_blocked(link, slot))
                {
                        prior = at;
                        at = next;
                        continue;
                }

                address_to prior_out = prior;
                return at;
        }

        return WATERLINK_NONE;
}

/*
        The acknowledgement a receiver owes, as one record per key: how far
        the key is delivered, the highest sequence seen at all, and which of
        the sixty four sequences after the first missing one are held back
        waiting for it. The first frees everything up to it at the sender.
        The third tells the sender those frames arrived and are only waiting,
        so they leave the flight -- they are no longer on the path -- and
        what is left in flight on that key is what really went missing. Without
        it a lost frame at the head of one key's stream sits behind a full
        window of its own successors, and only the timer finds it.
*/
#define WATERLINK_ACK_RECORD 24
#define WATERLINK_ACK_MASK 64

static fn waterlink_ack_owe(struct waterlink_link address_to link, p32 key_at)
{
        struct waterlink_live address_to live = link->receiving + key_at;

        if (live->acking || link->acks >= WATERLINK_ACKS)
                return;

        if (!link->acks)
                link->owed = link->clock;
        live->acking = 1;
        link->acking[link->acks++] = live->key;
}

static positive waterlink_ack_write(struct waterlink_link address_to link,
                                    p8 address_to bytes, positive room)
{
        struct waterlink_frame head;
        positive fits;
        positive written = 0;
        positive taken = 0;

        if (!link->acks || room < WATERLINK_HEADER + WATERLINK_ACK_RECORD)
                return 0;

        fits = (room - WATERLINK_HEADER) / WATERLINK_ACK_RECORD;

        while (taken < link->acks && written < fits)
        {
                p64 key = link->acking[taken++];
                p32 key_at = waterlink_key_find(link->receiving, key);
                p8 address_to record;

                if (key_at == WATERLINK_NONE)
                        continue;

                record = bytes + WATERLINK_HEADER +
                         written * WATERLINK_ACK_RECORD;
                {
                        struct waterlink_live address_to live =
                                link->receiving + key_at;
                        p64 mask = 0;

                        for (p32 held = live->first; held != WATERLINK_NONE;
                             held = link->held[held].next)
                        {
                                p32 gap = link->held[held].head.sequence -
                                          live->sequence - 2;

                                if (link->held[held].head.sequence >
                                            live->sequence + 1 &&
                                    gap < WATERLINK_ACK_MASK)
                                        mask |= 1ull << gap;
                        }

                        memory_copy(record, address_of key, 8);
                        memory_copy(record + 8, address_of live->sequence, 4);
                        memory_copy(record + 12, address_of live->floor, 4);
                        memory_copy(record + 16, address_of mask, 8);
                        live->acking = 0;
                }
                written++;
        }

        link->acks -= (p32)taken;
        if (link->acks)
                memory_copy(link->acking, link->acking + taken,
                            link->acks * sizeof(p64));

        if (!written)
                return 0;

        memory_zero(address_of head, sizeof head);
        head.flags = WATERLINK_FRAME_ACK;
        head.length = (p16)(written * WATERLINK_ACK_RECORD);
        memory_copy(bytes, address_of head, WATERLINK_HEADER);
        return WATERLINK_HEADER + written * WATERLINK_ACK_RECORD;
}

/*
        Fill one datagram body: what acknowledgements are owed, then as many
        whole frames as the path allows and the body fits.

        Sets alone when the body holds an urgent frame. An urgent frame cannot
        wait in a segment run for the forty frames behind it -- a run is built
        by waiting, and waiting is the thing this link exists to refuse -- so
        the caller sends that datagram by itself and pays the unbatched price
        on purpose. For the same reason an urgent frame is not held for the
        window or the pacer: it is a few bytes, and the person typing it is
        the one who notices.

        Frames already queued still ride along behind the urgent one when the
        path allows. They were not going to leave sooner in any case, the
        datagram is padded to a fixed size whether they are in it or not, and
        the urgent frame is in front of them.

        Acknowledgements are not held by the window either: they are what
        opens it.

        Returns the bytes written, or zero when there is nothing to send now.
*/
static bool waterlink_ack_due(struct waterlink_link address_to link, p64 now)
{
        return link->acks &&
               (link->owed_now || link->owed_count >= WATERLINK_ACK_EVERY ||
                now - link->owed >= WATERLINK_ACK_DELAY);
}

positive waterlink_fill(struct waterlink_link address_to link,
                        address_any out, p64 now, bool address_to alone)
{
        p8 address_to bytes = (p8 address_to)out;
        positive used = 0;
        positive room = WATERLINK_PAYLOAD;
        positive paced = 0;
        bool probed = false;
        bool full = false;

        address_to alone = false;
        link->carried = 0;
        if (now > link->clock)
                link->clock = now;

        if (link->flight_head != WATERLINK_NONE)
                waterlink_losses(link, now);

        //      Room is kept at the end for the acknowledgements owed, so a
        //      full run of frames still carries them.
        if (link->acks)
                room -= WATERLINK_HEADER +
                        WATERLINK_ACK_RECORD *
                                (link->acks < 8 ? link->acks : 8);

        for (p32 band = 0; band < WATERLINK_BANDS && !full; band++)
        {
                for (;;)
                {
                        struct waterlink_frame head;
                        struct waterlink_slot address_to slot;
                        p32 prior = WATERLINK_NONE;
                        p32 key_at;
                        p32 at;

                        if (band != WATERLINK_BAND_URGENT && !link->probes &&
                            (link->in_flight >= link->window ||
                             (link->smoothed && now < link->pace)))
                                break;

                        at = waterlink_band_take(link, band, now,
                                                 address_of prior);
                        if (at == WATERLINK_NONE)
                                break;

                        slot = link->slot + at;
                        if (used + WATERLINK_HEADER + slot->length > room)
                        {
                                //      Only the acknowledgements' room stands
                                //      in a full frame's way: the frame goes
                                //      and they ride the next datagram. Kept
                                //      back instead, a full frame waited for
                                //      the acknowledgement to fall due -- a
                                //      millisecond of a stream stopped, with
                                //      wake saying now the whole time.
                                if (!used && WATERLINK_HEADER + slot->length <=
                                                     WATERLINK_PAYLOAD)
                                        room = WATERLINK_PAYLOAD;
                                else
                                {
                                        full = true;
                                        break;
                                }
                        }

                        head.key = slot->key;
                        head.sequence = slot->sequence;
                        head.follows = slot->follows;
                        head.channel = slot->channel;
                        head.length = slot->length;
                        head.deadline = slot->deadline;
                        head.flags = slot->flags;
                        head.inflated = slot->inflated;
                        head.reserved = 0;

                        memory_copy(bytes + used, address_of head,
                                    WATERLINK_HEADER);
                        used += WATERLINK_HEADER;
                        if (slot->length)
                                memory_copy(bytes + used, slot->payload,
                                            slot->length);
                        used += slot->length;

                        if (band == WATERLINK_BAND_URGENT)
                                address_to alone = true;
                        else if (link->probes)
                                probed = true;
                        else
                                paced += WATERLINK_HEADER + slot->length;

                        waterlink_band_unlink(link, band, at, prior);
                        slot->state = WATERLINK_SLOT_FLIGHT;
                        slot->serial = ++link->serial;
                        slot->sent = now;
                        if (slot->tries++)
                                link->retransmitted++;
                        waterlink_flight_append(link, at);
                        link->in_flight += waterlink_bytes(slot);
                        link->sent++;

                        key_at = waterlink_key_find(link->sending, slot->key);
                        if (key_at != WATERLINK_NONE)
                                link->sending[key_at].flying++;
                }
        }

        if (probed)
                link->probes--;

        if (used)
                link->carried = 1;

        if (link->acks && (used || waterlink_ack_due(link, now)))
        {
                used += waterlink_ack_write(link, bytes + used,
                                            WATERLINK_PAYLOAD - used);
                if (!link->acks)
                {
                        link->owed_count = 0;
                        link->owed_now = 0;
                }
        }

        /*      The pacer: the bytes this datagram carried, as a share of a
                round trip at a quarter over the window's rate, with up to a
                burst of credit banked while the link sat idle. Charged by
                what was carried and not by the datagram: a datagram holding
                one small control frame charged as a full one held the next
                small frame a millisecond on a quiet link, which was every
                round of a flow-controlled stream. */
        if (paced && link->smoothed)
        {
                p64 gap = link->smoothed * paced * 4 / (link->window * 5);
                p64 whole = link->smoothed * WATERLINK_DATAGRAM * 4 /
                            (link->window * 5);
                p64 floor = now > whole * WATERLINK_PACE_BURST
                                    ? now - whole * WATERLINK_PACE_BURST
                                    : 0;

                if (link->pace < floor)
                        link->pace = floor;
                link->pace += gap;
        }

        return used;
}

/*
        When the caller should next call fill if nothing arrives first: now,
        when acknowledgements are owed or something may be sent; when the
        pacer next lets a datagram go; when the oldest frame in flight would
        time out. ~0 when there is nothing to wait for.
*/
p64 waterlink_wake(struct waterlink_link address_to link, p64 now)
{
        p64 wake = ~0ull;
        bool sendable = false;

        if (waterlink_ack_due(link, now) ||
            link->head[WATERLINK_BAND_URGENT] != WATERLINK_NONE)
                return now;

        if (link->acks)
                wake = link->owed + WATERLINK_ACK_DELAY;

        if (link->probes)
                for (p32 band = WATERLINK_BAND_TIMED; band < WATERLINK_BANDS;
                     band++)
                        if (link->head[band] != WATERLINK_NONE)
                                return now;

        //      A frame waiting on its key's window is waiting for an
        //      acknowledgement or the timer, and both of those wake the
        //      caller already; answering now for it would spin.
        for (p32 band = WATERLINK_BAND_TIMED; band < WATERLINK_BANDS &&
                                              !sendable;
             band++)
                for (p32 at = link->head[band]; at != WATERLINK_NONE;
                     at = link->slot[at].next)
                        if (!waterlink_key_blocked(link, link->slot + at))
                        {
                                sendable = true;
                                break;
                        }

        if (sendable && link->in_flight < link->window)
        {
                if (!link->smoothed || link->pace <= now)
                        return now;
                if (link->pace < wake)
                        wake = link->pace;
        }

        if (link->flight_head != WATERLINK_NONE)
        {
                p64 due = link->slot[link->flight_head].sent +
                          waterlink_timeout(link);

                if (due < wake)
                        wake = due;
        }

        return wake;
}

// Nothing queued, nothing in flight and nothing owed: the link can close.
bool waterlink_idle(struct waterlink_link address_to link)
{
        for (p32 band = 0; band < WATERLINK_BANDS; band++)
                if (link->head[band] != WATERLINK_NONE)
                        return false;

        return link->flight_head == WATERLINK_NONE && !link->acks;
}

/*
        One acknowledgement record, at the sender.

        Everything on the key up to what was delivered is done and freed. The
        highest seen marks one frame as arrived but waiting, which moves the
        latest known arrival forward without freeing anything -- the far side
        may yet drop it if its hold-back fills, and the timer still covers it.
*/
static fn waterlink_acknowledge(struct waterlink_link address_to link,
                                p64 key, p32 delivered, p64 mask,
                                p64 address_to newly, p64 address_to latest,
                                p64 address_to sample)
{
        p32 key_at = waterlink_key_find(link->sending, key);
        p32 at;

        if (key_at == WATERLINK_NONE)
                return;

        at = link->sending[key_at].first;
        while (at != WATERLINK_NONE)
        {
                struct waterlink_slot address_to slot = link->slot + at;
                p32 next = slot->chain;

                if (slot->sequence > delivered)
                {
                        p32 gap = slot->sequence - delivered - 2;

                        if (slot->sequence == delivered + 1)
                        {
                                at = next;
                                continue;
                        }
                        if (gap >= WATERLINK_ACK_MASK)
                                break;
                        if (mask & (1ull << gap))
                        {
                                if (slot->state == WATERLINK_SLOT_FLIGHT)
                                {
                                        if (slot->serial > address_to latest)
                                        {
                                                address_to latest =
                                                        slot->serial;
                                                address_to sample =
                                                        slot->tries == 1
                                                                ? link->clock -
                                                                          slot->sent
                                                                : 0;
                                        }
                                        address_to newly +=
                                                waterlink_bytes(slot);
                                        waterlink_flight_remove(link, at);
                                }
                                else if (slot->state == WATERLINK_SLOT_QUEUED)
                                        waterlink_band_remove(link, at);
                                slot->state = WATERLINK_SLOT_HELD;
                        }
                        at = next;
                        continue;
                }

                if (slot->state == WATERLINK_SLOT_FLIGHT)
                {
                        address_to newly += waterlink_bytes(slot);
                        if (slot->serial > address_to latest)
                        {
                                address_to latest = slot->serial;
                                address_to sample =
                                        slot->tries == 1 ? link->clock -
                                                                   slot->sent
                                                         : 0;
                        }
                        waterlink_flight_remove(link, at);
                }
                else if (slot->state == WATERLINK_SLOT_QUEUED)
                        waterlink_band_remove(link, at);

                link->acked++;
                //      Freeing may retire the key, which moves entries.
                waterlink_slot_free(link, at);
                key_at = waterlink_key_find(link->sending, key);
                if (key_at == WATERLINK_NONE)
                        return;
                at = next;
        }
}

static fn waterlink_estimate(struct waterlink_link address_to link,
                             p64 sample)
{
        if (!sample)
                sample = 1;
        link->recent = sample;

        if (!link->smoothed)
        {
                link->smoothed = sample;
                link->variance = sample / 2;
                return;
        }

        {
                p64 gap = sample > link->smoothed ? sample - link->smoothed
                                                  : link->smoothed - sample;

                link->variance = (3 * link->variance + gap) / 4;
                link->smoothed = (7 * link->smoothed + sample) / 8;
        }
}

static fn waterlink_acks_take(struct waterlink_link address_to link,
                              p8 address_to records, positive length, p64 now)
{
        p64 newly = 0;
        p64 latest = 0;
        p64 sample = 0;

        for (positive at = 0; at + WATERLINK_ACK_RECORD <= length;
             at += WATERLINK_ACK_RECORD)
        {
                p64 key;
                p32 delivered;
                p64 mask;

                memory_copy(address_of key, records + at, 8);
                memory_copy(address_of delivered, records + at + 8, 4);
                memory_copy(address_of mask, records + at + 16, 8);
                waterlink_acknowledge(link, key, delivered, mask,
                                      address_of newly, address_of latest,
                                      address_of sample);
        }

        if (latest)
        {
                if (latest > link->largest)
                        link->largest = latest;
                if (sample)
                        waterlink_estimate(link, sample);
                link->backoff = 0;
                link->probes = 0;
        }

        //      Growth only for what was sent after the last halving.
        if (newly && latest > link->recovery)
        {
                if (link->window < link->threshold)
                        link->window += newly;
                else
                        link->window += WATERLINK_DATAGRAM * newly /
                                        link->window + 1;
                if (link->window > WATERLINK_WINDOW_MOST)
                        link->window = WATERLINK_WINDOW_MOST;
        }

        if (link->flight_head != WATERLINK_NONE)
                waterlink_losses(link, now);
}

/*
        The receiving table is full of keys that are all still open, or of
        keys that ended: the one that ended longest ago gives its entry up.
*/
static p32 waterlink_receiving_make(struct waterlink_link address_to link,
                                    p64 key)
{
        p32 at = waterlink_key_make(link->receiving, key, 0);
        p32 oldest = WATERLINK_NONE;

        if (at != WATERLINK_NONE)
                return at;

        for (p32 look = 0; look < WATERLINK_KEYS; look++)
                if (link->receiving[look].over &&
                    (oldest == WATERLINK_NONE ||
                     link->receiving[look].ended <
                             link->receiving[oldest].ended))
                        oldest = look;

        if (oldest == WATERLINK_NONE)
                return WATERLINK_NONE;

        waterlink_key_retire(link->receiving, oldest);
        return waterlink_key_make(link->receiving, key, 0);
}

static fn waterlink_held_release(struct waterlink_link address_to link,
                                 p32 at)
{
        link->held[at].next = link->held_free;
        link->held_free = at;
}

/*
        Hold a frame back for the one it follows: in its key's chain by
        sequence, once. With the pool full it is dropped unheld, and the
        sender, which never heard of it arriving, sends it again.
*/
static bool waterlink_hold(struct waterlink_link address_to link,
                           struct waterlink_live address_to live,
                           struct waterlink_frame address_to head,
                           p8 address_to payload)
{
        p32 prior = WATERLINK_NONE;
        p32 look = live->first;
        p32 at;

        while (look != WATERLINK_NONE &&
               link->held[look].head.sequence < head->sequence)
        {
                prior = look;
                look = link->held[look].next;
        }

        if (look != WATERLINK_NONE &&
            link->held[look].head.sequence == head->sequence)
                return true;

        at = link->held_free;
        if (at == WATERLINK_NONE)
        {
                link->spilled++;
                return false;
        }

        link->held_free = link->held[at].next;
        link->held[at].head = address_to head;
        if (head->length)
                memory_copy(link->held[at].payload, payload, head->length);

        link->held[at].next = look;
        if (prior == WATERLINK_NONE)
                live->first = at;
        else
                link->held[prior].next = at;

        link->kept++;
        return true;
}

typedef fn (address_to waterlink_sink)(address_any context,
                                       struct waterlink_frame address_to head,
                                       p8 address_to payload);

static fn waterlink_hand(struct waterlink_link address_to link,
                         struct waterlink_live address_to live,
                         struct waterlink_frame address_to head,
                         p8 address_to payload, p64 now, waterlink_sink sink,
                         address_any context)
{
        live->sequence = head->sequence;
        if (sink)
                sink(context, head, payload);
        link->delivered++;

        if (head->flags & WATERLINK_FRAME_LAST)
        {
                live->over = 1;
                live->ended = now ? now : 1;
        }
}

/*
        After a delivery, what was waiting on it: the held frames of the key,
        oldest first, while each follows what has now been delivered. A held
        replaceable frame is passed over when the next held frame does not
        follow it -- that one is the newer value of the same state, and the
        old one has nothing left to say.
*/
static fn waterlink_release(struct waterlink_link address_to link,
                            struct waterlink_live address_to live, p64 now,
                            waterlink_sink sink, address_any context)
{
        while (live->first != WATERLINK_NONE && !live->over)
        {
                p32 at = live->first;
                struct waterlink_held address_to held = link->held + at;
                p32 next = held->next;

                if (held->head.sequence <= live->sequence)
                {
                        live->first = next;
                        waterlink_held_release(link, at);
                        link->stale++;
                        continue;
                }

                if (held->head.follows > live->sequence)
                        break;

                live->first = next;

                if ((held->head.flags & WATERLINK_FRAME_REPLACEABLE) &&
                    next != WATERLINK_NONE &&
                    link->held[next].head.follows < held->head.sequence &&
                    link->held[next].head.follows <= live->sequence)
                {
                        waterlink_held_release(link, at);
                        link->stale++;
                        continue;
                }

                waterlink_hand(link, live, address_of held->head,
                               held->payload, now, sink, context);
                waterlink_held_release(link, at);
        }

        //      An ended key keeps nothing.
        if (live->over)
                while (live->first != WATERLINK_NONE)
                {
                        p32 at = live->first;

                        live->first = link->held[at].next;
                        waterlink_held_release(link, at);
                }
}

/*
        Walk an authenticated body: acknowledgements to the sending half,
        frames to the application.

        Every bound is checked against what the body says it is, and nothing
        here trusts a length twice.

        A frame names the frame on its key it follows. When that one has been
        delivered, so is this; when it has not, this one waits in the
        hold-back for it. A frame at or behind what the key has delivered was
        already delivered, or is an older value of a state that was since
        replaced, and is dropped -- the receiving half of supersession, and
        what makes a copy the sender sent again arrive only once. Every frame
        that arrives, new or not, is owed an acknowledgement, because a copy
        arriving is the sender saying it never heard the first one was
        acknowledged.

        Returns false when the body is malformed, which for an authenticated
        peer means a bug on the far side rather than an attack -- and is still
        a refusal, because a link that guesses is a link with two opinions.
*/
bool waterlink_deliver_at(struct waterlink_link address_to link,
                          address_any body, positive length, p64 now,
                          waterlink_sink sink, address_any context)
{
        p8 address_to bytes = (p8 address_to)body;
        positive at = 0;
        bool framed = false;
        bool good;

        if (length > WATERLINK_PAYLOAD)
                return false;

        if (now > link->clock)
                link->clock = now;
        now = link->clock;

        while (at + WATERLINK_HEADER <= length)
        {
                struct waterlink_frame head;
                struct waterlink_live address_to live;
                p32 key_at;

                memory_copy(address_of head, bytes + at, WATERLINK_HEADER);

                //      A sealed box is padded with zeros to the full payload,
                //      and no real frame has zero flags, so a zero header is
                //      where the frames stop. The padding is inside the tag,
                //      so this is the sender's statement and not a guess.
                if (!head.flags && !head.key && !head.length)
                        break;

                at += WATERLINK_HEADER;

                if (head.reserved)
                        return false;

                if (at + head.length > length)
                        return false;

                if (head.flags == WATERLINK_FRAME_ACK)
                {
                        if (head.key || head.sequence || head.follows ||
                            head.channel || head.deadline || head.inflated ||
                            head.length % WATERLINK_ACK_RECORD)
                                return false;
                        waterlink_acks_take(link, bytes + at, head.length, now);
                        at += head.length;
                        continue;
                }

                if (!waterlink_frame_sane(head.flags, head.length,
                                          head.inflated) ||
                    !head.sequence || head.follows >= head.sequence)
                        return false;

                key_at = waterlink_receiving_make(link, head.key);
                if (key_at == WATERLINK_NONE)
                {
                        //      Every key is open: this one waits, unacknowledged.
                        link->refused++;
                        at += head.length;
                        continue;
                }

                live = link->receiving + key_at;
                waterlink_ack_owe(link, key_at);
                framed = true;
                if (head.flags & (WATERLINK_FRAME_URGENT | WATERLINK_FRAME_LAST))
                        link->owed_now = 1;

                if (head.sequence <= live->sequence || live->over)
                {
                        link->stale++;
                        link->owed_now = 1;
                        at += head.length;
                        continue;
                }

                if (head.follows > live->sequence)
                {
                        link->owed_now = 1;
                        if (waterlink_hold(link, live, address_of head,
                                           bytes + at) &&
                            head.sequence > live->floor)
                                live->floor = head.sequence;
                        at += head.length;
                        continue;
                }

                if (head.sequence > live->floor)
                        live->floor = head.sequence;

                waterlink_hand(link, live, address_of head, bytes + at, now,
                               sink, context);
                waterlink_release(link, live, now, sink, context);
                at += head.length;
        }

        //      Where the frames stop, the rest is the seal's padding and
        //      all zeros -- a tail too short for a header included, which
        //      every box of acknowledgements alone has, cut to whole blocks.
        for (good = true; at < length; at++)
                good = good && !bytes[at];
        if (good && framed)
                link->owed_count++;
        return good;
}

// The form that takes the time from the last call that gave one.
bool waterlink_deliver(struct waterlink_link address_to link,
                       address_any body, positive length,
                       waterlink_sink sink, address_any context)
{
        return waterlink_deliver_at(link, body, length, link->clock, sink,
                                    context);
}

/*
        Whether a datagram's counter is one this session has not seen.

        Accepting marks it, so this is asked once per datagram and only after
        the tag has verified -- a counter taken from an unauthenticated header
        would let anyone at all slide the window forward and lock the session
        out of its own traffic.

        The window does not move. A counter owns the bit at counter modulo the
        window width, forever, and advancing the top only clears the bits the
        top has just passed over. The first way I wrote this shifted the whole
        bitmap instead, which cost the same thirty two words whether the
        counter advanced by one or by a thousand, and cost them on every
        datagram that arrives in order -- which is nearly all of them.

        Clearing is the part that is wrong quietly. A bit is not free when the
        top moves past it: it still holds the answer for a counter exactly one
        window older, and that counter is outside the window now but its bit
        is in the way of the one arriving. So every slot between the old top
        and the new is cleared, in whole words where a word is crossed and by
        mask where it is not. Skip that and a session that runs long enough
        starts refusing its own traffic as a replay, once per window, forever.
*/
#define WATERLINK_REPLAY_WORDS (WATERLINK_REPLAY_WINDOW / 64)

_Static_assert((WATERLINK_REPLAY_WINDOW & (WATERLINK_REPLAY_WINDOW - 1)) == 0,
               "the replay window is a ring and its width must be a power of two");

bool waterlink_replay_new(struct waterlink_replay address_to window, p64 counter)
{
        p64 behind;
        positive slot;

        if (counter > window->top)
        {
                p64 step = counter - window->top;
                p64 at = window->top + 1;

                //      Counted by what is left to clear, not by at <= counter:
                //      a run that ends at the last counter there is wraps at
                //      to zero, and that test then never fails.
                if (step >= WATERLINK_REPLAY_WINDOW)
                        memory_zero(window->seen, sizeof(window->seen));
                else
                        while (step)
                        {
                                positive low;
                                positive high;
                                p64 span = step - 1;
                                p64 mask;

                                slot = (positive)(at &
                                                  (WATERLINK_REPLAY_WINDOW - 1));
                                low = slot & 63;
                                high = span >= (p64)(63 - low) ? 63
                                                               : low +
                                                                         (positive)span;

                                mask = high == 63 ? ~0ull
                                                  : (1ull << (high + 1)) - 1;
                                mask &= ~((1ull << low) - 1);
                                window->seen[slot >> 6] &= ~mask;

                                at += high - low + 1;
                                step -= high - low + 1;
                        }

                window->top = counter;
                slot = (positive)(counter & (WATERLINK_REPLAY_WINDOW - 1));
                window->seen[slot >> 6] |= 1ull << (slot & 63);
                return true;
        }

        behind = window->top - counter;
        if (behind >= WATERLINK_REPLAY_WINDOW)
                return false;

        slot = (positive)(counter & (WATERLINK_REPLAY_WINDOW - 1));
        if (window->seen[slot >> 6] & (1ull << (slot & 63)))
                return false;

        window->seen[slot >> 6] |= 1ull << (slot & 63);
        return true;
}

#endif // WATERLINK_LINK_INCLUDED
