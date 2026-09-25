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
        github.com/dawnlarsson/moonwater
*/

#ifndef WATERLINK_LINK_INCLUDED
#define WATERLINK_LINK_INCLUDED

#include "waterlink.c"

// The largest frame that can share a datagram with nothing else.
#define WATERLINK_FRAME_MAX (WATERLINK_PAYLOAD - WATERLINK_HEADER_MOST)

/*
        Ceilings, not guesses at a working set. A fixed count is what buys no
        allocation failure on the datapath and a cost that does not depend on
        what the far side does. If one is ever too low, raise it here -- that
        is the whole intent of naming it.

        SLOTS is what may be queued or unacknowledged at once, so it is also
        the most a link can have in flight: 256 frames of 1149 bytes is 294 KB
        a round trip, 14 MB/s at 20 ms and far past a gigabyte a second over a
        loopback's tens of microseconds. A key may run KEY_WINDOW frames past
        its oldest one not yet taken, and HELD is what a receiver keeps for
        keys that are ahead of what was taken -- out of order, or waiting for
        the application -- so it holds two streams running full at once, which
        is as many as the service ever receives on.
*/
#define WATERLINK_SLOTS 256
#define WATERLINK_HELD 128
#define WATERLINK_KEY_WINDOW 64

#define WATERLINK_NONE 0xffffffffu

/*
        Two queues, because urgency is a band and not a sort. Within a band
        post order is already the order a receiver will want, so a band is a
        list, a frame joins the end of its band's list, and supersession
        replaces a frame where it stands rather than moving it.
*/
#define WATERLINK_BAND_URGENT 0
#define WATERLINK_BAND_NORMAL 1
#define WATERLINK_BANDS 2

#define WATERLINK_SLOT_FREE 0
#define WATERLINK_SLOT_QUEUED 1 // in its band, waiting to be sent
#define WATERLINK_SLOT_FLIGHT 2 // sent, waiting for its acknowledgement
#define WATERLINK_SLOT_HELD 3   // arrived, and the far side holds it

struct waterlink_slot {
        p64 sent;   // the last transmission
        p64 serial; // the last transmission's place in the link's order
        p32 sequence;
        p32 next;  // the next slot in its band or in the flight
        p32 prior; // the one before it in the flight
        p32 chain; // the next slot on the same key, by sequence
        p16 length;
        p8 key;
        p8 flags;
        p8 state;
        p8 tries;
        p8 payload[WATERLINK_FRAME_MAX];
};

/*
        One key, each way. A key is a number below WATERLINK_KEYS, so these
        are tables indexed by it: no hashing, and nothing to evict.

        Sending: the next sequence, the class its first frame gave it, and
        the chain of its slots from oldest to newest -- the newest is the only
        one supersession ever asks about.

        Receiving: how far the key has been taken, the frames held back for
        it, and whether the application said "not now".
*/
struct waterlink_sending {
        p32 sequence;
        p32 first;
        p32 last;
        p16 flying;
        p8 class;   // WATERLINK_FRAME_REPLACEABLE or _DURABLE, 0 until posted
        p8 closing; // its last frame is posted
};

struct waterlink_receiving {
        p32 delivered;
        p32 first;
        p8 over;   // its last frame was taken
        p8 paused; // the application would not take the next one
};

// A frame that arrived ahead of what was taken, kept until it can be.
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
        p64 seen[WATERLINK_REPLAY_WINDOW / 64 + 1];
};

_Static_assert(WATERLINK_KEYS <= 64, "the keys owing an acknowledgement are one word");

struct waterlink_link {
        struct waterlink_slot slot[WATERLINK_SLOTS];
        struct waterlink_held held[WATERLINK_HELD];
        struct waterlink_sending sending[WATERLINK_KEYS];
        struct waterlink_receiving receiving[WATERLINK_KEYS];
        struct waterlink_replay replay;
        p64 acking; // the keys owed an acknowledgement, a bit each

        p32 head[WATERLINK_BANDS];
        p32 tail[WATERLINK_BANDS];
        p32 requeue[WATERLINK_BANDS]; // where the next lost frame goes back in
        p32 free;
        p32 free_count;
        p32 held_free;
        p32 flight_head;
        p32 flight_tail;

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
        p32 backoff;
        p32 probes;        // datagrams the probe timer lets past the window

        // What the caller may want to know without instrumenting the caller.
        p64 posted;
        p64 superseded;
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
        for (p32 key = 0; key < WATERLINK_KEYS; key++)
        {
                link->sending[key].sequence = 1;
                link->sending[key].first = WATERLINK_NONE;
                link->sending[key].last = WATERLINK_NONE;
                link->receiving[key].first = WATERLINK_NONE;
        }
        for (p32 band = 0; band < WATERLINK_BANDS; band++)
        {
                link->head[band] = WATERLINK_NONE;
                link->tail[band] = WATERLINK_NONE;
                link->requeue[band] = WATERLINK_NONE;
        }

        link->free_count = WATERLINK_SLOTS;
        link->flight_head = WATERLINK_NONE;
        link->flight_tail = WATERLINK_NONE;
        link->window = WATERLINK_WINDOW_FIRST;
        link->threshold = ~0ull;
}

static p32 waterlink_band_of(p8 flags)
{
        return flags & WATERLINK_FRAME_URGENT ? WATERLINK_BAND_URGENT
                                              : WATERLINK_BAND_NORMAL;
}

// A slot's frame as it goes out, and what it costs the window.
static positive waterlink_head_put(p8 address_to at,
                                   struct waterlink_slot address_to slot)
{
        positive used = 2;

        at[0] = slot->flags;
        at[1] = slot->key;
        used += memory_vli_put(at + used, slot->sequence);
        used += memory_vli_put(at + used, slot->length);
        return used;
}

static p64 waterlink_bytes(struct waterlink_slot address_to slot)
{
        return 2 + memory_vli_size(slot->sequence) +
               memory_vli_size(slot->length) + slot->length;
}

/*
        Whether a frame's flags are ones this link will carry at all: one
        class, since a frame that is both replaceable and durable has no
        answer to "may this be dropped", and no bit it does not know. The
        acknowledgement flag is the link's own and no caller may set it.
*/
static bool waterlink_frame_sane(p8 flags)
{
        bool replaceable = (flags & WATERLINK_FRAME_REPLACEABLE) != 0;
        bool durable = (flags & WATERLINK_FRAME_DURABLE) != 0;

        return replaceable != durable && !(flags & ~WATERLINK_FRAME_WIRE);
}

// The flight is a list in the order things were sent.
static fn waterlink_flight_append(struct waterlink_link address_to link, p32 at)
{
        struct waterlink_slot address_to slot = link->slot + at;

        slot->next = WATERLINK_NONE;
        slot->prior = link->flight_tail;
        if (link->flight_tail == WATERLINK_NONE)
                link->flight_head = at;
        else
                link->slot[link->flight_tail].next = at;
        link->flight_tail = at;
        link->in_flight += waterlink_bytes(slot);
        link->sending[slot->key].flying++;
}

static fn waterlink_flight_remove(struct waterlink_link address_to link,
                                  p32 at)
{
        struct waterlink_slot address_to slot = link->slot + at;

        if (slot->prior == WATERLINK_NONE)
                link->flight_head = slot->next;
        else
                link->slot[slot->prior].next = slot->next;
        if (slot->next == WATERLINK_NONE)
                link->flight_tail = slot->prior;
        else
                link->slot[slot->next].prior = slot->prior;

        link->in_flight -= waterlink_bytes(slot);
        link->sending[slot->key].flying--;
        slot->next = WATERLINK_NONE;
        slot->prior = WATERLINK_NONE;
}

static fn waterlink_band_append(struct waterlink_link address_to link, p32 at)
{
        p32 band = waterlink_band_of(link->slot[at].flags);

        link->slot[at].state = WATERLINK_SLOT_QUEUED;
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

// Off the band or the flight, whichever it is on.
static fn waterlink_slot_unqueue(struct waterlink_link address_to link, p32 at)
{
        if (link->slot[at].state == WATERLINK_SLOT_FLIGHT)
                waterlink_flight_remove(link, at);
        else if (link->slot[at].state == WATERLINK_SLOT_QUEUED)
                waterlink_band_remove(link, at);
}

/*
        A lost frame goes back ahead of everything posted since and behind
        every frame lost before it that has not gone out again yet: the front
        of a band is a queue of retransmissions in the order they were found.
        Putting the newest loss first instead starves the oldest -- which is
        the one the far side is holding everything else back for.

        Except the oldest itself: a key's first frame not yet taken goes in
        front of every other retransmission. The far side's hold-back is one
        pool for every key, so frames later on a key can fill it, be refused,
        time out and come back first forever, with the window full of them
        and the one frame that would empty the pool never reaching the front
        -- a bulk transfer on a clean path stalled six seconds that way.
*/
static fn waterlink_band_requeue(struct waterlink_link address_to link, p32 at)
{
        p32 band = waterlink_band_of(link->slot[at].flags);
        p32 after = link->requeue[band];

        link->slot[at].state = WATERLINK_SLOT_QUEUED;
        if (link->sending[link->slot[at].key].first == at)
        {
                link->slot[at].next = link->head[band];
                link->head[band] = at;
                if (link->tail[band] == WATERLINK_NONE)
                        link->tail[band] = at;
                if (after == WATERLINK_NONE)
                        link->requeue[band] = at;
                return;
        }
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

// A slot leaves its key's chain, already off the band and the flight.
static fn waterlink_slot_free(struct waterlink_link address_to link, p32 at)
{
        struct waterlink_slot address_to slot = link->slot + at;
        struct waterlink_sending address_to live = link->sending + slot->key;
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

        slot->state = WATERLINK_SLOT_FREE;
        slot->chain = WATERLINK_NONE;
        slot->next = link->free;
        link->free = at;
        link->free_count++;
}

/*
        Queue a frame for sending.

        A key's class is the class of its first frame, and a frame of the
        other class on it is refused: a stream key carries every frame once
        and in order, a register key only its newest value. That one rule is
        what lets a frame name no other frame. The supersession rule lives
        here: a frame on a register key whose newest frame is still in the
        same band takes that frame's slot under the next sequence -- replaced
        where it stands if queued, sent again in its place if it had already
        gone. The far side takes any value newer than the one it has.

        Returns false and counts a refusal when the frame is malformed, the
        queue is full, the key ran out of sequence numbers, or the key's last
        frame was already posted. A full queue is the caller's signal to stop
        producing, not this file's to start choosing.
*/
bool waterlink_post(struct waterlink_link address_to link, p8 key, p8 flags,
                    address_any payload, p16 length, p64 now)
{
        struct waterlink_sending address_to live;
        p8 class = flags & (WATERLINK_FRAME_REPLACEABLE | WATERLINK_FRAME_DURABLE);
        struct waterlink_slot address_to slot;
        bool queued;
        p32 at;

        /*      Form no pointer from an application key until its bound is
                proved.  Merely computing sending + key outside the array is
                undefined in C, even when the short-circuit below keeps it
                from being dereferenced. */
        if (key >= WATERLINK_KEYS || !waterlink_frame_sane(flags) ||
            length > WATERLINK_FRAME_MAX)
        {
                link->refused++;
                return false;
        }
        live = link->sending + key;
        if ((live->class && live->class != class) || live->closing ||
            live->sequence == WATERLINK_NONE)
        {
                link->refused++;
                return false;
        }
        if (now > link->clock)
                link->clock = now;
        live->class = class;

        at = live->last;
        if (class == WATERLINK_FRAME_REPLACEABLE && at != WATERLINK_NONE &&
            waterlink_band_of(link->slot[at].flags) == waterlink_band_of(flags))
                link->superseded++;
        else
        {
                at = link->free;
                if (at == WATERLINK_NONE)
                {
                        link->refused++;
                        return false;
                }
                link->free = link->slot[at].next;
                link->free_count--;
                link->slot[at].state = WATERLINK_SLOT_FREE;
                link->slot[at].chain = WATERLINK_NONE;
                if (live->last == WATERLINK_NONE)
                        live->first = at;
                else
                        link->slot[live->last].chain = at;
                live->last = at;
                link->posted++;
        }

        slot = link->slot + at;
        queued = slot->state == WATERLINK_SLOT_QUEUED;
        if (!queued)
                waterlink_slot_unqueue(link, at);
        slot->key = key;
        slot->sequence = live->sequence++;
        slot->sent = 0;
        slot->serial = 0;
        slot->prior = WATERLINK_NONE;
        slot->tries = 0;
        slot->length = length;
        slot->flags = flags;
        if (length)
                memory_copy(slot->payload, payload, length);
        if (!queued)
                waterlink_band_append(link, at);
        if (flags & WATERLINK_FRAME_LAST)
                live->closing = 1;
        return true;
}

// Slots free now, which is how much a caller may read before it posts.
p32 waterlink_room(struct waterlink_link address_to link)
{
        return link->free_count;
}

/*
        The probe timer, from the estimate: before any sample, the first
        guess; after, the smoothed round trip, four deviations (a millisecond
        at least) and the longest the far side may hold an acknowledgement,
        doubled for each expiry since the last acknowledgement. Leave the
        acknowledgement delay out and a sender that stops -- at the end of a
        burst, or waiting on the far side -- times out on every last datagram
        the far side is still entitled to sit on: 223 expiries in a 100 MB
        stream over loopback, at 71 MB/s.
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

static fn waterlink_lose(struct waterlink_link address_to link, p32 at)
{
        waterlink_flight_remove(link, at);
        link->lost++;
        waterlink_band_requeue(link, at);
}

/*
        What is in flight and cannot have arrived: sent three transmissions
        or more before one that did, or sent before one that did and older
        than an eighth over the round trip. Lost frames go back to the front
        of their bands. One halving per round of loss, found by the
        transmission number: a loss of something sent before the last halving
        is the same congestion the halving already answered.

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

                if (slot->serial >= link->largest)
                        break;
                if (slot->serial + WATERLINK_REORDER <= link->largest ||
                    now - slot->sent >= threshold)
                {
                        if (slot->serial > link->recovery)
                                waterlink_congested(link);
                        waterlink_lose(link, at);
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
                        waterlink_lose(link, link->flight_head);
                link->threshold = link->window / 2 > WATERLINK_WINDOW_LEAST
                                          ? link->window / 2
                                          : WATERLINK_WINDOW_LEAST;
                link->window = WATERLINK_WINDOW_LEAST;
                link->recovery = link->serial;
        }
        else
                waterlink_lose(link, at);

        link->probes = 2;
}

/*
        A key may run no more than its window ahead of its oldest frame the
        far side has not taken -- counting what it holds as well as what is
        on the path, since held frames are what fill its pool, and what an
        application that is not reading keeps held is how it slows the
        sender: the acknowledgement is the credit.
*/
static bool waterlink_key_blocked(struct waterlink_link address_to link,
                                  struct waterlink_slot address_to slot)
{
        p32 first = link->sending[slot->key].first;

        return first != WATERLINK_NONE &&
               slot->sequence - link->slot[first].sequence >=
                       WATERLINK_KEY_WINDOW;
}

/*
        Take the next frame worth sending from a band, passing over a key
        that already has a full window out. The slot before the one returned
        comes back too, so the caller can unlink it without walking again.
*/
static p32 waterlink_band_take(struct waterlink_link address_to link, p32 band,
                               p32 address_to prior_out)
{
        p32 prior = WATERLINK_NONE;

        for (p32 at = link->head[band]; at != WATERLINK_NONE;
             at = link->slot[at].next)
        {
                if (band == WATERLINK_BAND_URGENT ||
                    !waterlink_key_blocked(link, link->slot + at))
                {
                        address_to prior_out = prior;
                        return at;
                }
                prior = at;
        }
        return WATERLINK_NONE;
}

/*
        The acknowledgement a receiver owes, as one frame per key: how far
        the key has been taken, and which of the sixty four sequences after
        that it holds. The first frees everything up to it at the sender. The
        second tells the sender those frames arrived and are only waiting --
        out of order, or for an application that said "not now" -- so they
        leave the flight and are not sent again, and what is left in flight
        on that key is what really went missing.
*/
#define WATERLINK_ACK_MASK 64

static fn waterlink_ack_owe(struct waterlink_link address_to link, p8 key)
{
        if (!link->acking)
                link->owed = link->clock;
        link->acking |= 1ull << key;
}

static positive waterlink_ack_write(struct waterlink_link address_to link,
                                    p8 address_to bytes, positive room)
{
        positive used = 0;

        while (link->acking)
        {
                p8 key = (p8)bits_trailing_zeros(link->acking);
                struct waterlink_receiving address_to live =
                        link->receiving + key;
                p64 mask = 0;

                for (p32 held = live->first; held != WATERLINK_NONE;
                     held = link->held[held].next)
                {
                        p32 gap = link->held[held].head.sequence -
                                  live->delivered - 1;

                        if (gap < WATERLINK_ACK_MASK)
                                mask |= 1ull << gap;
                }

                if (used + 2 + memory_vli_size(live->delivered) +
                            memory_vli_size(mask) >
                    room)
                        break;
                bytes[used++] = WATERLINK_FRAME_ACK;
                bytes[used++] = key;
                used += memory_vli_put(bytes + used, live->delivered);
                used += memory_vli_put(bytes + used, mask);
                link->acking &= link->acking - 1;
        }
        return used;
}

static bool waterlink_ack_due(struct waterlink_link address_to link, p64 now)
{
        return link->acking &&
               (link->owed_now || link->owed_count >= WATERLINK_ACK_EVERY ||
                now - link->owed >= WATERLINK_ACK_DELAY);
}

/*
        Fill one datagram body: as many whole frames as the path allows and
        the body fits, then what acknowledgements are owed.

        Sets alone when the body holds an urgent frame. An urgent frame cannot
        wait in a segment run for the forty frames behind it -- a run is built
        by waiting, and waiting is the thing this link exists to refuse -- so
        the caller sends that datagram by itself and pays the unbatched price
        on purpose. For the same reason an urgent frame is not held for the
        window or the pacer: it is a few bytes, and the person typing it is
        the one who notices. Frames already queued still ride along behind
        it when the path allows; they were not going to leave sooner.

        Acknowledgements are not held by the window either: they are what
        opens it. Room is kept for a few, so a full run of frames still
        carries them.

        Returns the bytes written, or zero when there is nothing to send now.
*/
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
        if (now > link->clock)
                link->clock = now;

        if (link->flight_head != WATERLINK_NONE)
                waterlink_losses(link, now);

        if (link->acking)
        {
                positive owed = (positive)bits_counted(link->acking);

                room -= WATERLINK_ACK_MOST * (owed < 8 ? owed : 8);
        }

        for (p32 band = 0; band < WATERLINK_BANDS && !full; band++)
        {
                for (;;)
                {
                        struct waterlink_slot address_to slot;
                        p32 prior = WATERLINK_NONE;
                        p32 at;
                        positive size;

                        if (band != WATERLINK_BAND_URGENT && !link->probes &&
                            (link->in_flight >= link->window ||
                             (link->smoothed && now < link->pace)))
                                break;

                        at = waterlink_band_take(link, band, address_of prior);
                        if (at == WATERLINK_NONE)
                                break;

                        slot = link->slot + at;
                        size = (positive)waterlink_bytes(slot);
                        if (used + size > room)
                        {
                                //      Only the acknowledgements' room stands
                                //      in a full frame's way: the frame goes
                                //      and they ride the next datagram. Kept
                                //      back instead, a full frame waited for
                                //      the acknowledgement to fall due -- a
                                //      millisecond of a stream stopped, with
                                //      wake saying now the whole time.
                                if (!used && size <= WATERLINK_PAYLOAD)
                                        room = WATERLINK_PAYLOAD;
                                else
                                {
                                        full = true;
                                        break;
                                }
                        }

                        used += waterlink_head_put(bytes + used, slot);
                        if (slot->length)
                                memory_copy(bytes + used, slot->payload,
                                            slot->length);
                        used += slot->length;

                        if (band == WATERLINK_BAND_URGENT)
                                address_to alone = true;
                        else if (link->probes)
                                probed = true;
                        else
                                paced += size;

                        waterlink_band_unlink(link, band, at, prior);
                        slot->state = WATERLINK_SLOT_FLIGHT;
                        slot->serial = ++link->serial;
                        slot->sent = now;
                        if (slot->tries++)
                                link->retransmitted++;
                        waterlink_flight_append(link, at);
                        link->sent++;
                }
        }

        if (probed)
                link->probes--;

        if (link->acking && (used || waterlink_ack_due(link, now)))
        {
                used += waterlink_ack_write(link, bytes + used,
                                            WATERLINK_PAYLOAD - used);
                if (!link->acking)
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

        if (link->acking)
                wake = link->owed + WATERLINK_ACK_DELAY;

        //      A frame waiting on its key's window is waiting for an
        //      acknowledgement or the timer, and both of those wake the
        //      caller already; answering now for it would spin.
        for (p32 at = link->head[WATERLINK_BAND_NORMAL];
             at != WATERLINK_NONE && !sendable; at = link->slot[at].next)
                sendable = !waterlink_key_blocked(link, link->slot + at);

        if (sendable && (link->probes || link->in_flight < link->window))
        {
                if (link->probes || !link->smoothed || link->pace <= now)
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
        return link->head[WATERLINK_BAND_URGENT] == WATERLINK_NONE &&
               link->head[WATERLINK_BAND_NORMAL] == WATERLINK_NONE &&
               link->flight_head == WATERLINK_NONE && !link->acking;
}

/*
        One acknowledgement, at the sender. Everything on the key up to what
        was taken is done and freed. A frame the mask says is held has
        arrived and is waiting, which moves the latest known arrival forward
        without freeing anything -- the far side still has to take it, and
        the slot stays until it does.
*/
static fn waterlink_acknowledge(struct waterlink_link address_to link, p8 key,
                                p32 delivered, p64 mask, p64 address_to newly,
                                p64 address_to latest, p64 address_to sample)
{
        p32 at = link->sending[key].first;

        while (at != WATERLINK_NONE)
        {
                struct waterlink_slot address_to slot = link->slot + at;
                p32 next = slot->chain;
                p32 gap = slot->sequence - delivered - 1;
                bool taken = slot->sequence <= delivered;

                if (!taken && (gap >= WATERLINK_ACK_MASK ||
                               !(mask & (1ull << gap))))
                {
                        at = next;
                        continue;
                }
                if (slot->state == WATERLINK_SLOT_FLIGHT)
                {
                        address_to newly += waterlink_bytes(slot);
                        if (slot->serial > address_to latest)
                        {
                                address_to latest = slot->serial;
                                address_to sample = slot->tries == 1
                                                            ? link->clock - slot->sent
                                                            : 0;
                        }
                }
                waterlink_slot_unqueue(link, at);
                if (taken)
                {
                        link->acked++;
                        waterlink_slot_free(link, at);
                }
                else
                        slot->state = WATERLINK_SLOT_HELD;
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

/*
        What a body's acknowledgements add up to, applied once for the body:
        the latest arrival and a round trip from it, the window grown by what
        was newly delivered, and the losses that makes visible.
*/
static fn waterlink_acks_settle(struct waterlink_link address_to link,
                                p64 newly, p64 latest, p64 sample, p64 now)
{
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
        Hold a frame for its key: in the key's chain by sequence, once. With
        the pool full it is dropped unheld, and the sender, which never heard
        of it arriving, sends it again.
*/
static fn waterlink_hold(struct waterlink_link address_to link,
                         struct waterlink_receiving address_to live,
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
                return;

        at = link->held_free;
        if (at == WATERLINK_NONE)
        {
                link->spilled++;
                return;
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
}

typedef bool (address_to waterlink_sink)(address_any context,
                                         struct waterlink_frame address_to head,
                                         p8 address_to payload);

// Hand one frame to the application; false when it would not take it.
static bool waterlink_hand(struct waterlink_link address_to link,
                           struct waterlink_receiving address_to live,
                           struct waterlink_frame address_to head,
                           p8 address_to payload, waterlink_sink sink,
                           address_any context)
{
        if (sink && !sink(context, head, payload))
                return false;
        live->delivered = head->sequence;
        link->delivered++;
        if (head->flags & WATERLINK_FRAME_LAST)
                live->over = 1;
        return true;
}

static fn waterlink_held_drop(struct waterlink_link address_to link,
                              struct waterlink_receiving address_to live)
{
        p32 at = live->first;

        live->first = link->held[at].next;
        link->held[at].next = link->held_free;
        link->held_free = at;
}

/*
        What a key holds, handed on while it can be: a stream's frames while
        each is the next, a register's newest value only. When the
        application will not take one the key pauses there, held, until
        waterlink_resume. An ended key keeps nothing.
*/
static fn waterlink_release(struct waterlink_link address_to link, p8 key,
                            waterlink_sink sink, address_any context)
{
        struct waterlink_receiving address_to live = link->receiving + key;

        while (live->first != WATERLINK_NONE && !live->over)
        {
                struct waterlink_held address_to held = link->held + live->first;
                bool newer = held->next != WATERLINK_NONE;

                if (held->head.sequence <= live->delivered ||
                    ((held->head.flags & WATERLINK_FRAME_REPLACEABLE) && newer))
                {
                        link->stale++;
                        waterlink_held_drop(link, live);
                        continue;
                }
                if ((held->head.flags & WATERLINK_FRAME_DURABLE) &&
                    held->head.sequence != live->delivered + 1)
                        break;
                if (!waterlink_hand(link, live, address_of held->head,
                                    held->payload, sink, context))
                {
                        live->paused = 1;
                        return;
                }
                waterlink_held_drop(link, live);
        }
        live->paused = 0;
        while (live->over && live->first != WATERLINK_NONE)
                waterlink_held_drop(link, live);
}

/*
        The application will take a paused key's frames again: what it holds
        goes on now, and the sender hears at once, since what it hears is
        what lets it send more.
*/
fn waterlink_resume(struct waterlink_link address_to link, p8 key,
                    waterlink_sink sink, address_any context)
{
        /*      Keys also arrive from the application side of the link.  The
                wire judge confines its byte before it reaches this API, but
                a confused or compromised service must not turn an invalid
                channel number into an out-of-bounds receiving-table access. */
        if (key >= WATERLINK_KEYS)
                return;

        p32 before = link->receiving[key].delivered;

        waterlink_release(link, key, sink, context);
        if (link->receiving[key].delivered != before)
        {
                waterlink_ack_owe(link, key);
                link->owed_now = 1;
        }
}

bool waterlink_paused(struct waterlink_link address_to link, p8 key)
{
        return key < WATERLINK_KEYS && link->receiving[key].paused;
}

/*
        What a judged body holds, one part to each frame or acknowledgement
        in the order they came: its flags and key, a frame's sequence or
        what an acknowledgement says was delivered, a frame's length or an
        acknowledgement's mask, and where a frame's payload starts. The
        smallest part is four bytes, so a body holds at most a quarter of
        its bytes of them.
*/
struct waterlink_part
{
        p8 flags;
        p8 key;
        p16 at;
        p32 number;
        p64 more;
};

#define WATERLINK_PARTS (WATERLINK_PAYLOAD / 4)

/*
        Judge an authenticated body whole, and say what it holds: the number
        of parts written, or -1 when the link would not take it.

        A tag proves who sent the bytes, not that they are a body, and a body
        is taken whole or not at all -- a request followed by a malformed
        frame must not reach the application while the datagram as a whole
        is refused. So every frame and acknowledgement is read and checked
        here, and nothing but zeros may follow the last: a zero flags byte is
        where the frames stop, and the padding is inside the tag, so this is
        the sender's statement and not a guess. What is read is kept, so the
        body is read once and applied from the parts.

        One pass, in each machine's registers: a part is two stores, a number
        of one byte never leaves the line, and the zeros after the last part
        are lib.c's memory_span_byte.
*/
bipolar waterlink_judge(address_any body, positive length,
                        struct waterlink_part address_to parts);

_Static_assert(sizeof(struct waterlink_part) == 16 &&
                       __builtin_offsetof(struct waterlink_part, at) == 2 &&
                       __builtin_offsetof(struct waterlink_part, number) == 4 &&
                       __builtin_offsetof(struct waterlink_part, more) == 8,
               "the judge writes a part as two words");
_Static_assert(WATERLINK_PAYLOAD == 1168 && WATERLINK_FRAME_MAX == 1159 &&
                       WATERLINK_KEYS == 64 && WATERLINK_FRAME_ACK == 8 &&
                       WATERLINK_FRAME_WIRE == 0x17 &&
                       WATERLINK_NONE == 0xffffffffu,
               "the judge's bounds are these");

#if X64
/*
        rdi the body, rsi its length, rdx the parts; r8 the offset, r9 sixteen
        times the parts written, ebx the flags and key as one word, r10 and
        r11 the two numbers.
*/
__asm__(
    ASM_FUNC(waterlink_judge)
    "cmp $1168, %rsi\n   ja 8f\n"
    "push %rbx\n   xor %r8d, %r8d\n   xor %r9d, %r9d\n"
    //  A part starts at a flags byte that is not zero, with its key after.
    "1:  lea 1(%r8), %rax\n   cmp %rsi, %rax\n   jae 6f\n"
    "movzwl (%rdi,%r8), %ebx\n   test %bl, %bl\n   jz 6f\n"
    "cmp $0x3fff, %ebx\n   ja 9f\n"
    "add $2, %r8\n"
    "cmp %rsi, %r8\n   jae 9f\n"
    "movzbl (%rdi,%r8), %r10d\n   inc %r8\n   test %r10b, %r10b\n   js 20f\n"
    "2:  cmp %rsi, %r8\n   jae 9f\n"
    "movzbl (%rdi,%r8), %r11d\n   inc %r8\n   test %r11b, %r11b\n   js 30f\n"
    //  The part: flags, key, where a payload would start and the first
    //  number in one word, the second in the other.
    "3:  mov %r8, %rax\n   shl $16, %rax\n   or %rbx, %rax\n"
    "mov %r10, %rcx\n   shl $32, %rcx\n   or %rcx, %rax\n"
    "mov %rax, (%rdx,%r9)\n   mov %r11, 8(%rdx,%r9)\n   add $16, %r9\n"
    //  An acknowledgement names a sequence; its mask is any word.
    "cmp $8, %bl\n   jne 4f\n"
    "mov $0xfffffffe, %eax\n   cmp %rax, %r10\n   jbe 1b\n   jmp 9f\n"
    //  A frame: a sequence, one class, no bit the wire does not know, and a
    //  length that is a frame's and is there.
    "4:  lea -1(%r10), %rax\n   mov $0xfffffffd, %ecx\n   cmp %rcx, %rax\n   ja 9f\n"
    "mov %ebx, %eax\n   shr $1, %eax\n   xor %ebx, %eax\n   test $1, %al\n   jz 9f\n"
    "test $0xe8, %bl\n   jnz 9f\n"
    "cmp $1159, %r11\n   ja 9f\n"
    "mov %rsi, %rax\n   sub %r8, %rax\n   cmp %rax, %r11\n   ja 9f\n"
    "add %r11, %r8\n   jmp 1b\n"
    //  Zeros to the end. rbx keeps sixteen times the count over the bytes
    //  left, both under 4096 times it, across the call.
    "6:  sub %r8, %rsi\n   add %r8, %rdi\n   shl $12, %r9\n   lea (%r9,%rsi), %rbx\n"
    "mov %rsi, %rdx\n   xor %esi, %esi\n   call memory_span_byte\n"
    "mov %ebx, %ecx\n   and $4095, %ecx\n   cmp %rcx, %rax\n   jne 9f\n"
    "mov %rbx, %rax\n   shr $16, %rax\n   pop %rbx\n"
    ASM_RET
    "9:  pop %rbx\n"
    "8:  mov $-1, %rax\n"
    ASM_RET
    //  The rest of a number that said more follows, seven bits a byte; a
    //  last byte of zero is a longer spelling of a shorter number, and the
    //  tenth byte holds bit 63 alone.
    "20: and $0x7f, %r10d\n   mov $7, %ecx\n"
    "21: cmp %rsi, %r8\n   jae 9b\n"
    "movzbl (%rdi,%r8), %eax\n   inc %r8\n"
    "cmp $63, %ecx\n   je 24f\n"
    "test %al, %al\n   js 23f\n"
    "test %eax, %eax\n   jz 9b\n"
    "shl %cl, %rax\n   or %rax, %r10\n   jmp 2b\n"
    "23: and $0x7f, %eax\n   shl %cl, %rax\n   or %rax, %r10\n   add $7, %ecx\n   jmp 21b\n"
    "24: cmp $1, %eax\n   jne 9b\n   bts $63, %r10\n   jmp 2b\n"
    "30: and $0x7f, %r11d\n   mov $7, %ecx\n"
    "31: cmp %rsi, %r8\n   jae 9b\n"
    "movzbl (%rdi,%r8), %eax\n   inc %r8\n"
    "cmp $63, %ecx\n   je 34f\n"
    "test %al, %al\n   js 33f\n"
    "test %eax, %eax\n   jz 9b\n"
    "shl %cl, %rax\n   or %rax, %r11\n   jmp 3b\n"
    "33: and $0x7f, %eax\n   shl %cl, %rax\n   or %rax, %r11\n   add $7, %ecx\n   jmp 31b\n"
    "34: cmp $1, %eax\n   jne 9b\n   bts $63, %r11\n   jmp 3b\n"
    ASM_END(waterlink_judge)
);
#elif ARM64
/*
        x0 the body, x1 its length, x2 the parts; x8 the offset, x9 the parts
        written, w11 the flags, w12 the key, x13 and x14 the two numbers.
*/
__asm__(
    ASM_FUNC(waterlink_judge)
    "cmp x1, #1168\n   b.hi 8f\n"
    "mov x8, #0\n   mov x9, #0\n"
    "1:  add x10, x8, #1\n   cmp x10, x1\n   b.hs 6f\n"
    "ldrb w11, [x0, x8]\n   cbz w11, 6f\n"
    "ldrb w12, [x0, x10]\n   cmp w12, #63\n   b.hi 8f\n"
    "add x8, x8, #2\n"
    "cmp x8, x1\n   b.hs 8f\n"
    "ldrb w13, [x0, x8]\n   add x8, x8, #1\n   tbnz w13, #7, 20f\n"
    "2:  cmp x8, x1\n   b.hs 8f\n"
    "ldrb w14, [x0, x8]\n   add x8, x8, #1\n   tbnz w14, #7, 30f\n"
    "3:  orr w15, w11, w12, lsl #8\n   orr x15, x15, x8, lsl #16\n"
    "orr x15, x15, x13, lsl #32\n"
    "add x16, x2, x9, lsl #4\n   stp x15, x14, [x16]\n   add x9, x9, #1\n"
    "cmp w11, #8\n   b.ne 4f\n"
    "mov w16, #0xfffffffe\n   cmp x13, x16\n   b.ls 1b\n   b 8f\n"
    "4:  sub x16, x13, #1\n   mov w17, #0xfffffffd\n   cmp x16, x17\n   b.hi 8f\n"
    "eor w16, w11, w11, lsr #1\n   tbz w16, #0, 8f\n"
    "mov w17, #0x17\n   bics wzr, w11, w17\n   b.ne 8f\n"
    "cmp x14, #1159\n   b.hi 8f\n"
    "sub x16, x1, x8\n   cmp x14, x16\n   b.hi 8f\n"
    "add x8, x8, x14\n   b 1b\n"
    "6:  stp x29, x30, [sp, #-32]!\n   mov x29, sp\n   stp x19, x20, [sp, #16]\n"
    "sub x19, x1, x8\n   mov x20, x9\n"
    "add x0, x0, x8\n   mov w1, #0\n   mov x2, x19\n   bl memory_span_byte\n"
    "cmp x0, x19\n   csinv x0, x20, xzr, eq\n"
    "ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #32\n"
    ASM_RET
    "8:  mov x0, #-1\n"
    ASM_RET
    "20: and w13, w13, #0x7f\n   mov w17, #7\n"
    "21: cmp x8, x1\n   b.hs 8b\n"
    "ldrb w16, [x0, x8]\n   add x8, x8, #1\n"
    "cmp w17, #63\n   b.eq 24f\n"
    "tbnz w16, #7, 23f\n"
    "cbz w16, 8b\n"
    "lsl x16, x16, x17\n   orr x13, x13, x16\n   b 2b\n"
    "23: and w16, w16, #0x7f\n   lsl x16, x16, x17\n   orr x13, x13, x16\n"
    "add w17, w17, #7\n   b 21b\n"
    "24: cmp w16, #1\n   b.ne 8b\n   orr x13, x13, #0x8000000000000000\n   b 2b\n"
    "30: and w14, w14, #0x7f\n   mov w17, #7\n"
    "31: cmp x8, x1\n   b.hs 8b\n"
    "ldrb w16, [x0, x8]\n   add x8, x8, #1\n"
    "cmp w17, #63\n   b.eq 34f\n"
    "tbnz w16, #7, 33f\n"
    "cbz w16, 8b\n"
    "lsl x16, x16, x17\n   orr x14, x14, x16\n   b 3b\n"
    "33: and w16, w16, #0x7f\n   lsl x16, x16, x17\n   orr x14, x14, x16\n"
    "add w17, w17, #7\n   b 31b\n"
    "34: cmp w16, #1\n   b.ne 8b\n   orr x14, x14, #0x8000000000000000\n   b 3b\n"
    ASM_END(waterlink_judge)
);
#elif RISCV64
/*
        a0 the body, a1 its length, a2 the parts; t1 the offset, t2 the parts
        written, t4 the flags, t5 the key, a3 and a4 the two numbers.
*/
__asm__(
    ASM_FUNC(waterlink_judge)
    "li t0, 1168\n   bgtu a1, t0, 8f\n"
    "li t1, 0\n   li t2, 0\n"
    "1:  addi t0, t1, 1\n   bgeu t0, a1, 6f\n"
    "add t3, a0, t1\n   lbu t4, 0(t3)\n   beqz t4, 6f\n"
    "lbu t5, 1(t3)\n   li t0, 63\n   bgtu t5, t0, 8f\n"
    "addi t1, t1, 2\n"
    "bgeu t1, a1, 8f\n"
    "add t3, a0, t1\n   lbu a3, 0(t3)\n   addi t1, t1, 1\n"
    "andi t0, a3, 0x80\n   bnez t0, 20f\n"
    "2:  bgeu t1, a1, 8f\n"
    "add t3, a0, t1\n   lbu a4, 0(t3)\n   addi t1, t1, 1\n"
    "andi t0, a4, 0x80\n   bnez t0, 30f\n"
    "3:  slli t0, t5, 8\n   or t0, t0, t4\n   slli t3, t1, 16\n   or t0, t0, t3\n"
    "slli t3, a3, 32\n   or t0, t0, t3\n"
    "slli t3, t2, 4\n   add t3, a2, t3\n   sd t0, 0(t3)\n   sd a4, 8(t3)\n"
    "addi t2, t2, 1\n"
    "li t0, 8\n   bne t4, t0, 4f\n"
    "li t0, 0xfffffffe\n   bleu a3, t0, 1b\n   j 8f\n"
    "4:  addi t0, a3, -1\n   li t3, 0xfffffffd\n   bgtu t0, t3, 8f\n"
    "srli t0, t4, 1\n   xor t0, t0, t4\n   andi t0, t0, 1\n   beqz t0, 8f\n"
    "andi t0, t4, 0xe8\n   bnez t0, 8f\n"
    "li t0, 1159\n   bgtu a4, t0, 8f\n"
    "sub t0, a1, t1\n   bgtu a4, t0, 8f\n"
    "add t1, t1, a4\n   j 1b\n"
    "6:  addi sp, sp, -32\n   sd ra, 24(sp)\n   sd s0, 16(sp)\n   sd s1, 8(sp)\n"
    "sub s0, a1, t1\n   mv s1, t2\n"
    "add a0, a0, t1\n   li a1, 0\n   mv a2, s0\n   call memory_span_byte\n"
    "li t0, -1\n   bne a0, s0, 7f\n   mv t0, s1\n"
    "7:  mv a0, t0\n"
    "ld ra, 24(sp)\n   ld s0, 16(sp)\n   ld s1, 8(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    "8:  li a0, -1\n"
    ASM_RET
    "20: andi a3, a3, 0x7f\n   li a5, 7\n"
    "21: bgeu t1, a1, 8b\n"
    "add t3, a0, t1\n   lbu t0, 0(t3)\n   addi t1, t1, 1\n"
    "li t3, 63\n   beq a5, t3, 24f\n"
    "andi t3, t0, 0x80\n   bnez t3, 23f\n"
    "beqz t0, 8b\n"
    "sll t0, t0, a5\n   or a3, a3, t0\n   j 2b\n"
    "23: andi t0, t0, 0x7f\n   sll t0, t0, a5\n   or a3, a3, t0\n"
    "addi a5, a5, 7\n   j 21b\n"
    "24: li t3, 1\n   bne t0, t3, 8b\n   slli t0, t0, 63\n   or a3, a3, t0\n   j 2b\n"
    "30: andi a4, a4, 0x7f\n   li a5, 7\n"
    "31: bgeu t1, a1, 8b\n"
    "add t3, a0, t1\n   lbu t0, 0(t3)\n   addi t1, t1, 1\n"
    "li t3, 63\n   beq a5, t3, 34f\n"
    "andi t3, t0, 0x80\n   bnez t3, 33f\n"
    "beqz t0, 8b\n"
    "sll t0, t0, a5\n   or a4, a4, t0\n   j 3b\n"
    "33: andi t0, t0, 0x7f\n   sll t0, t0, a5\n   or a4, a4, t0\n"
    "addi a5, a5, 7\n   j 31b\n"
    "34: li t3, 1\n   bne t0, t3, 8b\n   slli t0, t0, 63\n   or a4, a4, t0\n   j 3b\n"
    ASM_END(waterlink_judge)
);
#endif

/*
        Apply a judged body: acknowledgements to the sending half, frames to
        the application.

        A stream frame is taken when it is the next on its key and held when
        it is ahead; a register frame is taken when it is newer than what the
        key has. A frame at or behind what the key has taken was already
        taken, or is an older value, and is dropped -- what makes a copy the
        sender sent again arrive only once. Every frame that arrives, new or
        not, is owed an acknowledgement, because a copy arriving is the
        sender saying it never heard the first one was acknowledged.

        Every part comes from waterlink_judge, which has checked each bound
        against what the body says it is; nothing here reads the body but a
        frame's payload where the judge found it.
*/
fn waterlink_apply(struct waterlink_link address_to link, address_any body,
                   struct waterlink_part address_to parts, positive count,
                   p64 now, waterlink_sink sink, address_any context)
{
        p8 address_to bytes = (p8 address_to)body;
        bool framed = false;
        bool acked = false;
        p64 newly = 0, latest = 0, sample = 0;

        if (now > link->clock)
                link->clock = now;
        now = link->clock;

        for (struct waterlink_part address_to part = parts; part < parts + count;
             part++)
        {
                p8 flags = part->flags;
                p8 key = part->key;
                p8 address_to payload = bytes + part->at;
                struct waterlink_frame head;
                struct waterlink_receiving address_to live;

                if (flags == WATERLINK_FRAME_ACK)
                {
                        waterlink_acknowledge(link, key, part->number, part->more,
                                              address_of newly,
                                              address_of latest,
                                              address_of sample);
                        acked = true;
                        continue;
                }

                head.key = key;
                head.sequence = part->number;
                head.length = (p16)part->more;
                head.flags = flags;
                live = link->receiving + key;

                waterlink_ack_owe(link, key);
                framed = true;
                if (flags & (WATERLINK_FRAME_URGENT | WATERLINK_FRAME_LAST))
                        link->owed_now = 1;

                if (head.sequence <= live->delivered || live->over)
                {
                        link->stale++;
                        link->owed_now = 1;
                }
                else if (live->paused ||
                         ((flags & WATERLINK_FRAME_DURABLE) &&
                          head.sequence != live->delivered + 1))
                {
                        link->owed_now = 1;
                        waterlink_hold(link, live, address_of head, payload);
                }
                else if (!waterlink_hand(link, live, address_of head, payload,
                                         sink, context))
                {
                        waterlink_hold(link, live, address_of head, payload);
                        live->paused = 1;
                }
                else
                        waterlink_release(link, key, sink, context);
        }

        if (acked)
                waterlink_acks_settle(link, newly, latest, sample, now);

        if (framed)
                link->owed_count++;
}

// A body judged and then applied: what every caller without a judge wants.
bool waterlink_deliver(struct waterlink_link address_to link,
                          address_any body, positive length, p64 now,
                          waterlink_sink sink, address_any context)
{
        struct waterlink_part parts[WATERLINK_PARTS];
        bipolar count = waterlink_judge(body, length, parts);

        if (count < 0)
                return false;
        waterlink_apply(link, body, parts, (positive)count, now, sink, context);
        return true;
}

/*
        Whether a datagram's counter is one this session has not seen.

        Accepting marks it, so this is asked once per datagram and only after
        the tag has verified -- a counter taken from an unauthenticated header
        would let anyone at all slide the window forward and lock the session
        out of its own traffic.

        RFC 6479's window: a ring of 64-bit blocks, each counter's bit in the
        block its high bits name. The top moving forward clears whole blocks
        between the old top's and the new one's -- one spare block past the
        window's width is what lets clearing be whole blocks and still keep
        every counter the window promises. Nothing shifts, so an arrival in
        order costs one word.
*/
#define WATERLINK_REPLAY_BLOCKS (WATERLINK_REPLAY_WINDOW / 64 + 1)

_Static_assert(WATERLINK_REPLAY_WINDOW % 64 == 0,
               "the replay window is whole blocks");

bool waterlink_replay_new(struct waterlink_replay address_to window, p64 counter)
{
        p64 block = counter >> 6;
        p64 bit = 1ull << (counter & 63);
        p64 address_to word = window->seen + block % WATERLINK_REPLAY_BLOCKS;

        if (counter > window->top)
        {
                p64 top = window->top >> 6;
                p64 steps = block - top < WATERLINK_REPLAY_BLOCKS
                                    ? block - top
                                    : WATERLINK_REPLAY_BLOCKS;

                for (p64 step = 1; step <= steps; step++)
                        window->seen[(top + step) % WATERLINK_REPLAY_BLOCKS] = 0;
                window->top = counter;
        }
        else if (window->top - counter >= WATERLINK_REPLAY_WINDOW ||
                 (address_to word & bit))
                return false;

        address_to word |= bit;
        return true;
}

#endif // WATERLINK_LINK_INCLUDED
