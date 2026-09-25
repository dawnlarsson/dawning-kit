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

// What a slot's frame costs the window.
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
static KEEP fn waterlink_losses(struct waterlink_link address_to link, p64 now)
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

        In each machine's registers: a slot's fields are read once, at the
        widths post wrote them, before anything is written, so no read waits
        on a store; a frame's size is spelled once and is what the window is
        charged; a frame's head is built in a register and stored whole; and
        a call that has no frame to send touches only the acknowledgements.
        Nothing is written past the bytes returned.
*/
positive waterlink_fill(struct waterlink_link address_to link,
                        address_any out, p64 now, bool address_to alone);

_Static_assert(sizeof(struct waterlink_slot) == 1200 &&
                       __builtin_offsetof(struct waterlink_slot, serial) == 8 &&
                       __builtin_offsetof(struct waterlink_slot, sequence) == 16 &&
                       __builtin_offsetof(struct waterlink_slot, next) == 20 &&
                       __builtin_offsetof(struct waterlink_slot, prior) == 24 &&
                       __builtin_offsetof(struct waterlink_slot, length) == 32 &&
                       __builtin_offsetof(struct waterlink_slot, key) == 34 &&
                       __builtin_offsetof(struct waterlink_slot, flags) == 35 &&
                       __builtin_offsetof(struct waterlink_slot, state) == 36 &&
                       __builtin_offsetof(struct waterlink_slot, tries) == 37 &&
                       __builtin_offsetof(struct waterlink_slot, payload) == 38,
               "fill reads a slot at these places");
_Static_assert(sizeof(struct waterlink_held) == 1172 &&
                       __builtin_offsetof(struct waterlink_held, next) == 8 &&
                       sizeof(struct waterlink_sending) == 16 &&
                       __builtin_offsetof(struct waterlink_sending, first) == 4 &&
                       __builtin_offsetof(struct waterlink_sending, flying) == 12 &&
                       sizeof(struct waterlink_receiving) == 12 &&
                       __builtin_offsetof(struct waterlink_receiving, first) == 4,
               "fill reads the key tables at these places");
_Static_assert(__builtin_offsetof(struct waterlink_link, held) == 0x4b000 &&
                       __builtin_offsetof(struct waterlink_link, sending) == 0x6fa00 &&
                       __builtin_offsetof(struct waterlink_link, receiving) == 0x6fe00 &&
                       __builtin_offsetof(struct waterlink_link, acking) == 0x70210 &&
                       __builtin_offsetof(struct waterlink_link, head) == 0x70218 &&
                       __builtin_offsetof(struct waterlink_link, tail) == 0x70220 &&
                       __builtin_offsetof(struct waterlink_link, requeue) == 0x70228 &&
                       __builtin_offsetof(struct waterlink_link, flight_head) == 0x7023c &&
                       __builtin_offsetof(struct waterlink_link, flight_tail) == 0x70240 &&
                       __builtin_offsetof(struct waterlink_link, clock) == 0x70248 &&
                       __builtin_offsetof(struct waterlink_link, serial) == 0x70250 &&
                       __builtin_offsetof(struct waterlink_link, in_flight) == 0x70268 &&
                       __builtin_offsetof(struct waterlink_link, window) == 0x70270 &&
                       __builtin_offsetof(struct waterlink_link, smoothed) == 0x70280 &&
                       __builtin_offsetof(struct waterlink_link, pace) == 0x70290 &&
                       __builtin_offsetof(struct waterlink_link, owed) == 0x702a0 &&
                       __builtin_offsetof(struct waterlink_link, owed_count) == 0x702a8 &&
                       __builtin_offsetof(struct waterlink_link, owed_now) == 0x702ac &&
                       __builtin_offsetof(struct waterlink_link, probes) == 0x702b4 &&
                       __builtin_offsetof(struct waterlink_link, sent) == 0x702e0 &&
                       __builtin_offsetof(struct waterlink_link, retransmitted) == 0x702e8,
               "fill reads the link at these places");
_Static_assert(WATERLINK_PAYLOAD == 1168 && WATERLINK_ACK_MOST == 17 &&
                       WATERLINK_FRAME_MAX < 16384 && WATERLINK_BANDS == 2 &&
                       WATERLINK_KEY_WINDOW == 64 && WATERLINK_ACK_MASK == 64 &&
                       WATERLINK_ACK_EVERY == 2 && WATERLINK_ACK_DELAY == 1000 &&
                       WATERLINK_DATAGRAM * 4 == 4800 &&
                       WATERLINK_PACE_BURST == 16 && WATERLINK_SLOT_FLIGHT == 2,
               "fill's bounds are these");

#if X64
/*
        A number below 2^32 and over 127 in v, spelled seven bits a byte
        with the top bit saying more follows, and n its bytes. Each add moves
        the bits above a byte's seven up by one; the bytes that say more are
        the ones under the highest, a shift of 0x80808080. t and rcx go.
*/
#define WATERLINK_X64_SPELL(v, t, t32, n, n32)                                 \
    "mov %" v ", %" t "\n   and $-128, %" t "\n   add %" t ", %" v "\n"         \
    "mov %" v ", %" t "\n   and $-32768, %" t "\n   add %" t ", %" v "\n"       \
    "mov %" v ", %" t "\n   and $-8388608, %" t "\n   add %" t ", %" v "\n"     \
    "mov %" v ", %" t "\n   and $-2147483648, %" t "\n   add %" t ", %" v "\n"  \
    "bsr %" v ", %" n "\n   shr $3, %" n32 "\n"                                 \
    "lea 0(,%" n ",8), %ecx\n   neg %ecx\n   add $32, %ecx\n"                   \
    "mov $0x80808080, %" t32 "\n   shr %cl, %" t32 "\n   or %" t ", %" v "\n"   \
    "inc %" n32 "\n"

/*
        One frame out of a band: eax its slot, rsi the slot's address, r11d
        the slot before it in the band. edx the sequence and then the head,
        r8 the length, r9 the key, r10 the flags, rdi the sequence spelled,
        ecx its bytes, r14 the head's bytes, r15 the frame's. Everything the
        slot says is read before the body is written.
*/
#define WATERLINK_X64_FRAME(head, tail, requeue, full, again, carried)         \
    "mov 16(%rsi), %edx\n   movzwl 32(%rsi), %r8d\n"                           \
    "movzbl 34(%rsi), %r9d\n   movzbl 35(%rsi), %r10d\n"                       \
    "mov %edx, %edi\n   mov $1, %ecx\n   cmp $127, %edx\n   ja 50f\n"          \
    "51: mov %r8d, %edx\n   lea 3(%rcx), %r14d\n   cmp $127, %r8d\n   ja 52f\n" \
    "53: lea 16(,%rcx,8), %ecx\n   shl %cl, %rdx\n   shl $16, %rdi\n"          \
    "or %rdi, %rdx\n   mov %r9d, %edi\n   shl $8, %edi\n   or %edi, %r10d\n"   \
    "or %r10, %rdx\n"                                                          \
    "lea (%r14,%r8), %r15\n   lea (%r13,%r15), %rdi\n   cmp 8(%rsp), %rdi\n"   \
    "ja 54f\n"                                                                 \
    "55: lea (%rbp,%r13), %rdi\n   cmp $8, %r15\n   jb 56f\n"                  \
    "mov %rdx, (%rdi)\n   cmp $9, %r14d\n   je 57f\n"                          \
    /*  Off the band, into the flight. */                                      \
    "58: mov 20(%rsi), %ecx\n   cmp $-1, %r11d\n   je 59f\n"                   \
    "imul $1200, %r11, %rdx\n   mov %ecx, 20(%rbx,%rdx)\n   jmp 60f\n"          \
    "59: mov %ecx, " head "(%rbx)\n"                                           \
    "60: cmp %eax, " tail "(%rbx)\n   jne 61f\n   mov %r11d, " tail "(%rbx)\n"  \
    "61: cmp %eax, " requeue "(%rbx)\n   jne 62f\n"                            \
    "mov %r11d, " requeue "(%rbx)\n"                                           \
    "62: mov 0x70250(%rbx), %rcx\n   inc %rcx\n   mov %rcx, 0x70250(%rbx)\n"   \
    "mov %rcx, 8(%rsi)\n   mov %r12, (%rsi)\n   movb $2, 36(%rsi)\n"           \
    "movzbl 37(%rsi), %ecx\n   lea 1(%rcx), %edx\n   mov %dl, 37(%rsi)\n"      \
    "test %ecx, %ecx\n   jnz 63f\n"                                            \
    "64: mov 0x70240(%rbx), %ecx\n   movl $-1, 20(%rsi)\n   mov %ecx, 24(%rsi)\n" \
    "cmp $-1, %ecx\n   je 65f\n"                                               \
    "imul $1200, %rcx, %rcx\n   mov %eax, 20(%rbx,%rcx)\n   jmp 66f\n"          \
    "65: mov %eax, 0x7023c(%rbx)\n"                                            \
    "66: mov %eax, 0x70240(%rbx)\n   add %r15, 0x70268(%rbx)\n"                \
    "shl $4, %r9d\n   incw 0x6fa0c(%rbx,%r9)\n   incq 0x702e0(%rbx)\n"         \
    /*  The payload, read in the pieces post wrote it in. */                   \
    "test %r8d, %r8d\n   jz 67f\n"                                             \
    "lea (%rbp,%r13), %rdi\n   add %r14, %rdi\n   add $38, %rsi\n"             \
    "cmp $4, %r8d\n   jae 68f\n   cmp $2, %r8d\n   jae 69f\n"                  \
    "movzbl (%rsi), %eax\n   mov %al, (%rdi)\n   jmp 67f\n"                    \
    "69: movzwl (%rsi), %eax\n   movzwl -2(%rsi,%r8), %ecx\n"                  \
    "mov %ax, (%rdi)\n   mov %cx, -2(%rdi,%r8)\n   jmp 67f\n"                  \
    "68: cmp $8, %r8d\n   jae 70f\n"                                           \
    "mov (%rsi), %eax\n   mov -4(%rsi,%r8), %ecx\n"                            \
    "mov %eax, (%rdi)\n   mov %ecx, -4(%rdi,%r8)\n   jmp 67f\n"                \
    "70: cmp $16, %r8d\n   jae 71f\n"                                          \
    "mov (%rsi), %rax\n   mov -8(%rsi,%r8), %rcx\n"                            \
    "mov %rax, (%rdi)\n   mov %rcx, -8(%rdi,%r8)\n   jmp 67f\n"                \
    "71: cmp $32, %r8d\n   ja 72f\n"                                           \
    "mov (%rsi), %rax\n   mov 8(%rsi), %rcx\n"                                 \
    "mov -16(%rsi,%r8), %rdx\n   mov -8(%rsi,%r8), %r9\n"                      \
    "mov %rax, (%rdi)\n   mov %rcx, 8(%rdi)\n"                                 \
    "mov %rdx, -16(%rdi,%r8)\n   mov %r9, -8(%rdi,%r8)\n   jmp 67f\n"          \
    "72: mov %r11, 16(%rsp)\n   mov %r8, %rdx\n   call memory_copy_apart\n"    \
    "mov 16(%rsp), %r11\n"                                                     \
    "67: add %r15, %r13\n" carried "jmp " again "\n"                           \
    /*  A sequence over seven bits, a length over seven, no room. */           \
    "50: " WATERLINK_X64_SPELL("rdi", "r15", "r15d", "r14", "r14d")            \
    "mov %r14d, %ecx\n   jmp 51b\n"                                            \
    "52: mov %r8d, %edx\n   and $0x7f, %edx\n   or $0x80, %edx\n"              \
    "mov %r8d, %r14d\n   shr $7, %r14d\n   shl $8, %r14d\n   or %r14d, %edx\n" \
    "lea 4(%rcx), %r14d\n   jmp 53b\n"                                         \
    "54: test %r13, %r13\n   jnz " full "\n   cmp $1168, %r15\n   ja " full "\n" \
    "movq $1168, 8(%rsp)\n   jmp 55b\n"                                        \
    /*  A frame under eight bytes goes with its payload in the head's        \
        word, stored as two words that meet. */                                \
    "56: test %r8d, %r8d\n   jz 74f\n   cmp $2, %r8d\n   jae 75f\n"            \
    "movzbl 38(%rsi), %r10d\n   jmp 73f\n"                                     \
    "75: movzwl 38(%rsi), %r10d\n   movzwl 36(%rsi,%r8), %ecx\n"               \
    "cmp $3, %r8d\n   jne 73f\n   shl $8, %ecx\n   or %ecx, %r10d\n"           \
    "73: lea 0(,%r14,8), %ecx\n   shl %cl, %r10\n   or %r10, %rdx\n"           \
    "74: mov %edx, (%rdi)\n   lea -32(,%r15,8), %ecx\n   shr %cl, %rdx\n"      \
    "mov %edx, -4(%rdi,%r15)\n   xor %r8d, %r8d\n   jmp 58b\n"                 \
    "57: mov %r8d, %ecx\n   shr $7, %ecx\n   mov %cl, 8(%rdi)\n   jmp 58b\n"    \
    "63: incq 0x702e8(%rbx)\n   jmp 64b\n"

/*
        rdi the link, rsi the body, rdx now, rcx where alone goes. With a
        frame to send: rbx the link, rbp the body, r12 now, r13 the bytes
        used, and on the stack the bytes the normal band carried, the room,
        a band's prior across a copy, and alone's address. The
        acknowledgements are written with rdi, rsi and rdx as they came and
        r11 the bytes used.
*/
__asm__(
    ASM_FUNC(waterlink_fill)
    "movb $0, (%rcx)\n"
    "cmp %rdx, 0x70248(%rdi)\n   jae 1f\n   mov %rdx, 0x70248(%rdi)\n"
    "1:  cmpl $-1, 0x7023c(%rdi)\n   jne 2f\n"
    "cmpl $-1, 0x70218(%rdi)\n   jne 2f\n   cmpl $-1, 0x7021c(%rdi)\n   jne 2f\n"
    "xor %r11d, %r11d\n   jmp 100f\n"
    "2:  push %rbx\n   push %rbp\n   push %r12\n   push %r13\n   push %r14\n"
    "push %r15\n   sub $40, %rsp\n"
    "mov %rdi, %rbx\n   mov %rsi, %rbp\n   mov %rdx, %r12\n   mov %rcx, 24(%rsp)\n"
    "cmpl $-1, 0x7023c(%rbx)\n   je 5f\n"
    "mov %r12, %rsi\n   call waterlink_losses\n"
    //  Room kept for the acknowledgements owed, up to eight.
    "5:  xor %r13d, %r13d\n   movq $0, (%rsp)\n   mov $1168, %ecx\n"
    "mov 0x70210(%rbx), %rax\n   test %rax, %rax\n   jz 6f\n"
    "popcnt %rax, %rax\n   mov $8, %edx\n   cmp %rdx, %rax\n   cmova %rdx, %rax\n"
    "mov %rax, %rdx\n   shl $4, %rdx\n   add %rdx, %rax\n   sub %rax, %rcx\n"
    "6:  mov %rcx, 8(%rsp)\n"
    //  The urgent band, from its head.
    "10: mov 0x70218(%rbx), %eax\n   cmp $-1, %eax\n   je 20f\n"
    "imul $1200, %rax, %rsi\n   add %rbx, %rsi\n   mov $-1, %r11d\n"
    WATERLINK_X64_FRAME("0x70218", "0x70220", "0x70228", "19f", "10b", "")
    "19: mov 24(%rsp), %rax\n   movb $1, (%rax)\n   jmp 40f\n"
    "20: test %r13, %r13\n   jz 21f\n   mov 24(%rsp), %rax\n   movb $1, (%rax)\n"
    "21: mov $-1, %r11d\n"
    //  The normal band, while the window and the pacer allow, passing
    //  over a key with its window out; a frame taken leaves the ones
    //  passed over where they were, so the walk goes on from its prior.
    "30: cmpl $0, 0x702b4(%rbx)\n   jne 31f\n"
    "mov 0x70268(%rbx), %rax\n   cmp 0x70270(%rbx), %rax\n   jae 40f\n"
    "cmpq $0, 0x70280(%rbx)\n   je 31f\n   cmp 0x70290(%rbx), %r12\n   jb 40f\n"
    "31: cmp $-1, %r11d\n   je 32f\n"
    "imul $1200, %r11, %rax\n   mov 20(%rbx,%rax), %eax\n   jmp 33f\n"
    "32: mov 0x7021c(%rbx), %eax\n"
    "33: cmp $-1, %eax\n   je 40f\n"
    "imul $1200, %rax, %rsi\n   add %rbx, %rsi\n"
    "movzbl 34(%rsi), %ecx\n   shl $4, %ecx\n   mov 0x6fa04(%rbx,%rcx), %ecx\n"
    "cmp $-1, %ecx\n   je 34f\n"
    "imul $1200, %rcx, %rcx\n   mov 16(%rsi), %edx\n   sub 16(%rbx,%rcx), %edx\n"
    "cmp $63, %edx\n   jbe 34f\n"
    "mov %eax, %r11d\n   mov 20(%rsi), %eax\n   jmp 33b\n"
    "34:\n"
    WATERLINK_X64_FRAME("0x7021c", "0x70224", "0x7022c", "40f", "30b",
                        "add %r15, (%rsp)\n")
    //  A probe spends one expiry's worth; otherwise the pacer charges
    //  what the normal band carried.
    "40: mov 0x702b4(%rbx), %eax\n   mov (%rsp), %r15\n   test %eax, %eax\n"
    "jz 41f\n   test %r15, %r15\n   jz 42f\n   dec %eax\n   mov %eax, 0x702b4(%rbx)\n"
    "42: xor %r15d, %r15d\n"
    "41: test %r15, %r15\n   jz 43f\n"
    "mov 0x70280(%rbx), %rcx\n   test %rcx, %rcx\n   jz 43f\n"
    "mov 0x70270(%rbx), %rsi\n   lea (%rsi,%rsi,4), %rsi\n"
    "mov %rcx, %rax\n   imul %r15, %rax\n   shl $2, %rax\n   xor %edx, %edx\n"
    "div %rsi\n   mov %rax, %r8\n"
    "imul $4800, %rcx, %rax\n   xor %edx, %edx\n   div %rsi\n   shl $4, %rax\n"
    "mov %r12, %rcx\n   sub %rax, %rcx\n   xor %edx, %edx\n   cmp %rax, %r12\n"
    "cmovbe %rdx, %rcx\n"
    "mov 0x70290(%rbx), %rax\n   cmp %rcx, %rax\n   cmovb %rcx, %rax\n"
    "add %r8, %rax\n   mov %rax, 0x70290(%rbx)\n"
    "43: mov %rbx, %rdi\n   mov %rbp, %rsi\n   mov %r13, %r11\n   mov %r12, %rdx\n"
    "add $40, %rsp\n   pop %r15\n   pop %r14\n   pop %r13\n   pop %r12\n"
    "pop %rbp\n   pop %rbx\n"
    //  The acknowledgements, when frames go or they are due: per key, how
    //  far it was taken and a bit for each held past that. rdx the head,
    //  r8 the key's taken point, r9d its held chain and then the point's
    //  bytes, r10 the mask.
    "100: mov 0x70210(%rdi), %rax\n   test %rax, %rax\n   jz 119f\n"
    "test %r11, %r11\n   jnz 101f\n"
    "cmpb $0, 0x702ac(%rdi)\n   jne 101f\n   cmpl $2, 0x702a8(%rdi)\n   jae 101f\n"
    "sub 0x702a0(%rdi), %rdx\n   cmp $1000, %rdx\n   jb 119f\n"
    "101: bsf %rax, %rcx\n   lea (%rcx,%rcx,2), %r8\n   lea 0x6fe00(%rdi,%r8,4), %r8\n"
    "mov 4(%r8), %r9d\n   mov (%r8), %r8d\n   xor %r10d, %r10d\n"
    "mov %ecx, %edx\n   shl $8, %edx\n   or $8, %edx\n"
    "cmp $-1, %r9d\n   je 103f\n"
    "102: imul $1172, %r9, %rcx\n   add %rdi, %rcx\n"
    "mov 0x4b000(%rcx), %eax\n   sub %r8d, %eax\n   dec %eax\n"
    "mov 0x4b008(%rcx), %r9d\n   cmp $63, %eax\n   ja 104f\n   bts %rax, %r10\n"
    "104: cmp $-1, %r9d\n   jne 102b\n"
    "103: mov $1, %r9d\n   cmp $127, %r8d\n   ja 105f\n"
    "106: shl $16, %r8\n   or %r8, %rdx\n   cmp $127, %r10\n   ja 107f\n"
    "lea 16(,%r9,8), %ecx\n   shl %cl, %r10\n   or %r10, %rdx\n"
    "lea 3(%r9), %rcx\n   lea (%r11,%rcx), %rax\n   cmp $1168, %rax\n   ja 119f\n"
    "lea (%rsi,%r11), %r8\n   mov %rax, %r11\n   cmp $8, %ecx\n   je 108f\n"
    "mov %edx, (%r8)\n   lea -32(,%rcx,8), %ecx\n   shr %cl, %rdx\n"
    "mov %edx, -4(%rsi,%r11)\n   jmp 109f\n"
    "108: mov %rdx, (%r8)\n"
    "109: mov 0x70210(%rdi), %rax\n   lea -1(%rax), %rcx\n   and %rcx, %rax\n"
    "mov %rax, 0x70210(%rdi)\n   jnz 101b\n"
    "movl $0, 0x702a8(%rdi)\n   movb $0, 0x702ac(%rdi)\n"
    "119: mov %r11, %rax\n"
    ASM_RET
    "105: " WATERLINK_X64_SPELL("r8", "rax", "eax", "r9", "r9d")
    "jmp 106b\n"
    //  A mask over seven bits: the head a byte at a time, then the mask.
    "107: bsr %r10, %rcx\n   lea (%rcx,%rcx,8), %ecx\n   add $73, %ecx\n   shr $6, %ecx\n"
    "lea 2(%r9,%rcx), %rcx\n   lea (%r11,%rcx), %rax\n   cmp $1168, %rax\n   ja 119b\n"
    "lea (%rsi,%r11), %r8\n   mov %rax, %r11\n   lea 2(%r9), %ecx\n"
    "110: mov %dl, (%r8)\n   inc %r8\n   shr $8, %rdx\n   dec %ecx\n   jnz 110b\n"
    "111: cmp $127, %r10\n   jbe 112f\n   mov %r10d, %eax\n   or $0x80, %eax\n"
    "mov %al, (%r8)\n   inc %r8\n   shr $7, %r10\n   jmp 111b\n"
    "112: mov %r10b, (%r8)\n   jmp 109b\n"
    ASM_END(waterlink_fill)
);
#elif ARM64
/*
        The spelling as on x86_64, v a number over 127 below 2^32: t and u
        go, n its bytes.
*/
#define WATERLINK_A64_SPELL(v, t, tw, uw, n, nw)                               \
    "and " t ", " v ", #0xffffffffffffff80\n   add " v ", " v ", " t "\n"       \
    "and " t ", " v ", #0xffffffffffff8000\n   add " v ", " v ", " t "\n"       \
    "and " t ", " v ", #0xffffffffff800000\n   add " v ", " v ", " t "\n"       \
    "and " t ", " v ", #0xffffffff80000000\n   add " v ", " v ", " t "\n"       \
    "clz " n ", " v "\n   eor " n ", " n ", #63\n   lsr " n ", " n ", #3\n"      \
    "mov " uw ", #32\n   sub " uw ", " uw ", " nw ", lsl #3\n"                  \
    "mov " tw ", #0x80808080\n   lsr " tw ", " tw ", " uw "\n"                  \
    "orr " v ", " v ", " t "\n   add " nw ", " nw ", #1\n"

/*
        One frame: w10 its slot, x11 the slot's address, w8 the one before
        it in the band. w12 the sequence, w13 the length, w14 the key, x15
        the flags and then the head, w16 the sequence's bytes and then the
        frame's, w17 the head's. The slot's fields are read first, its
        bookkeeping written next, and the frame last.
*/
#define WATERLINK_A64_FRAME(head, tail, requeue, full, again, carried)         \
    "ldr w12, [x11, #16]\n   ldrh w13, [x11, #32]\n"                           \
    "ldrb w14, [x11, #34]\n   ldrb w15, [x11, #35]\n"                          \
    "mov w16, #1\n   cmp w12, #127\n   b.hi 50f\n"                             \
    "51: mov w9, w13\n   add w17, w16, #3\n   cmp w13, #127\n   b.hi 52f\n"    \
    "53: add w16, w16, #2\n   lsl w16, w16, #3\n   lsl x9, x9, x16\n"          \
    "orr x15, x15, x14, lsl #8\n   orr x15, x15, x12, lsl #16\n"               \
    "orr x15, x15, x9\n"                                                       \
    "add w16, w17, w13\n   add x12, x5, x16\n   cmp x12, x6\n   b.hi 54f\n"    \
    /*  Off the band, into the flight. */                                      \
    "55: ldr w12, [x11, #20]\n   mov w9, #1200\n   cmn w8, #1\n   b.eq 59f\n"  \
    "madd x9, x8, x9, x0\n   str w12, [x9, #20]\n   b 60f\n"                   \
    "59: str w12, [x4, #" head "]\n"                                           \
    "60: ldr w12, [x4, #" tail "]\n   cmp w12, w10\n   b.ne 61f\n"             \
    "str w8, [x4, #" tail "]\n"                                                \
    "61: ldr w12, [x4, #" requeue "]\n   cmp w12, w10\n   b.ne 62f\n"          \
    "str w8, [x4, #" requeue "]\n"                                             \
    "62: ldr x12, [x4, #0xa50]\n   add x12, x12, #1\n   str x12, [x4, #0xa50]\n" \
    "stp x2, x12, [x11]\n   mov w12, #2\n   strb w12, [x11, #36]\n"            \
    "ldrb w12, [x11, #37]\n   add w9, w12, #1\n   strb w9, [x11, #37]\n"       \
    "cbnz w12, 63f\n"                                                          \
    "64: ldr w12, [x4, #0xa40]\n   mov w9, #-1\n   stp w9, w12, [x11, #20]\n"  \
    "cmn w12, #1\n   b.eq 65f\n"                                               \
    "mov w9, #1200\n   madd x9, x12, x9, x0\n   str w10, [x9, #20]\n   b 66f\n" \
    "65: str w10, [x4, #0xa3c]\n"                                              \
    "66: str w10, [x4, #0xa40]\n"                                              \
    "ldr x12, [x4, #0xa68]\n   add x12, x12, x16\n   str x12, [x4, #0xa68]\n"   \
    "add x14, x4, x14, lsl #4\n   ldrh w12, [x14, #0x20c]\n"                   \
    "add w12, w12, #1\n   strh w12, [x14, #0x20c]\n"                           \
    "ldr x12, [x4, #0xae0]\n   add x12, x12, #1\n   str x12, [x4, #0xae0]\n"    \
    /*  The frame: the head in one store, the payload read in the pieces     \
        post wrote it in. */                                                   \
    "add x12, x1, x5\n   add x5, x5, x16\n" carried                            \
    "cmp w16, #8\n   b.lo 56f\n   str x15, [x12]\n   cmp w17, #9\n   b.eq 57f\n" \
    "58: cbz w13, 67f\n   add x12, x12, x17\n   add x11, x11, #38\n"           \
    "cmp w13, #4\n   b.hs 68f\n   cmp w13, #2\n   b.hs 69f\n"                  \
    "ldrb w9, [x11]\n   strb w9, [x12]\n   b 67f\n"                            \
    "69: sub x14, x13, #2\n   ldrh w9, [x11]\n   ldrh w10, [x11, x14]\n"       \
    "strh w9, [x12]\n   strh w10, [x12, x14]\n   b 67f\n"                      \
    "68: cmp w13, #8\n   b.hs 70f\n"                                           \
    "sub x14, x13, #4\n   ldr w9, [x11]\n   ldr w10, [x11, x14]\n"             \
    "str w9, [x12]\n   str w10, [x12, x14]\n   b 67f\n"                        \
    "70: cmp w13, #16\n   b.hs 71f\n"                                          \
    "sub x14, x13, #8\n   ldr x9, [x11]\n   ldr x10, [x11, x14]\n"             \
    "str x9, [x12]\n   str x10, [x12, x14]\n   b 67f\n"                        \
    "71: cmp w13, #32\n   b.hi 72f\n"                                          \
    "sub x14, x13, #16\n   ldr q0, [x11]\n   ldr q1, [x11, x14]\n"             \
    "str q0, [x12]\n   str q1, [x12, x14]\n   b 67f\n"                         \
    "72: stp x29, x30, [sp, #-80]!\n   mov x29, sp\n"                          \
    "stp x0, x1, [sp, #16]\n   stp x2, x3, [sp, #32]\n"                        \
    "stp x5, x6, [sp, #48]\n   stp x7, x8, [sp, #64]\n"                        \
    "mov x0, x12\n   mov x1, x11\n   mov x2, x13\n   bl memory_copy_apart\n"   \
    "ldp x0, x1, [sp, #16]\n   ldp x2, x3, [sp, #32]\n"                        \
    "ldp x5, x6, [sp, #48]\n   ldp x7, x8, [sp, #64]\n   ldp x29, x30, [sp], #80\n" \
    "add x4, x0, #0x6f, lsl #12\n   add x4, x4, #0x800\n"                      \
    "67: b " again "\n"                                                        \
    /*  A sequence over seven bits, a length over seven, no room. */           \
    "50: " WATERLINK_A64_SPELL("x12", "x9", "w9", "w17", "x16", "w16")         \
    "b 51b\n"                                                                  \
    "52: and w9, w13, #0x7f\n   orr w9, w9, #0x80\n   lsr w17, w13, #7\n"       \
    "orr w9, w9, w17, lsl #8\n   add w17, w16, #4\n   b 53b\n"                  \
    "54: cbnz x5, " full "\n   cmp w16, #1168\n   b.hi " full "\n"             \
    "mov x6, #1168\n   b 55b\n"                                                \
    /*  A frame under eight bytes goes with its payload in the head's        \
        word, stored as two words that meet. */                                \
    "56: cbz w13, 74f\n   cmp w13, #2\n   b.hs 75f\n   ldrb w9, [x11, #38]\n"  \
    "b 73f\n"                                                                  \
    "75: ldrh w9, [x11, #38]\n   cmp w13, #3\n   b.ne 73f\n"                   \
    "ldrb w10, [x11, #40]\n   orr w9, w9, w10, lsl #16\n"                      \
    "73: lsl w10, w17, #3\n   lsl x9, x9, x10\n   orr x15, x15, x9\n"          \
    "74: str w15, [x12]\n   sub w10, w16, #4\n   lsl w9, w10, #3\n"            \
    "lsr x15, x15, x9\n   str w15, [x12, x10]\n   b 67b\n"                     \
    "57: lsr w9, w13, #7\n   strb w9, [x12, #8]\n   b 58b\n"                    \
    "63: ldr x12, [x4, #0xae8]\n   add x12, x12, #1\n   str x12, [x4, #0xae8]\n" \
    "b 64b\n"

/*
        x0 the link, x1 the body, x2 now, x3 where alone goes, x4 the link
        plus 0x6f800 so every field past the slots is an offset from it, x5
        the bytes used, x6 the room, x7 the bytes the normal band carried,
        w8 a band's prior. Nothing is kept on the stack but across the two
        calls, the losses walk and a payload past thirty two bytes.
*/
__asm__(
    ASM_FUNC(waterlink_fill)
    "strb wzr, [x3]\n   add x4, x0, #0x6f, lsl #12\n   add x4, x4, #0x800\n"
    "ldr x10, [x4, #0xa48]\n   cmp x2, x10\n   b.ls 1f\n   str x2, [x4, #0xa48]\n"
    //  Nothing in flight and nothing queued: the acknowledgements alone.
    "1:  ldr w10, [x4, #0xa3c]\n   ldr w11, [x4, #0xa18]\n   ldr w12, [x4, #0xa1c]\n"
    "and w11, w11, w12\n   and w11, w11, w10\n   mov x5, #0\n   cmn w11, #1\n   b.eq 100f\n"
    "cmn w10, #1\n   b.eq 5f\n"
    "stp x29, x30, [sp, #-48]!\n   mov x29, sp\n   stp x0, x1, [sp, #16]\n"
    "stp x2, x3, [sp, #32]\n   mov x1, x2\n   bl waterlink_losses\n"
    "ldp x0, x1, [sp, #16]\n   ldp x2, x3, [sp, #32]\n   ldp x29, x30, [sp], #48\n"
    "add x4, x0, #0x6f, lsl #12\n   add x4, x4, #0x800\n"
    //  Room kept for the acknowledgements owed, up to eight.
    "5:  mov x5, #0\n   mov x6, #1168\n   mov x7, #0\n   ldr x10, [x4, #0xa10]\n"
    "cbz x10, 10f\n"
    "fmov d0, x10\n   cnt v0.8b, v0.8b\n   addv b0, v0.8b\n   fmov w10, s0\n"
    "mov w11, #8\n   cmp w10, #8\n   csel w10, w10, w11, lo\n"
    "add w10, w10, w10, lsl #4\n   sub x6, x6, x10\n"
    //  The urgent band, from its head.
    "10: ldr w10, [x4, #0xa18]\n   cmn w10, #1\n   b.eq 20f\n"
    "mov w9, #1200\n   madd x11, x10, x9, x0\n   mov w8, #-1\n"
    WATERLINK_A64_FRAME("0xa18", "0xa20", "0xa28", "19f", "10b", "")
    "19: mov w10, #1\n   strb w10, [x3]\n   b 40f\n"
    "20: cbz x5, 21f\n   mov w10, #1\n   strb w10, [x3]\n"
    "21: mov w8, #-1\n"
    //  The normal band, while the window and the pacer allow, passing over
    //  a key with its window out and going on from the prior.
    "30: ldr w10, [x4, #0xab4]\n   cbnz w10, 31f\n"
    "ldr x10, [x4, #0xa68]\n   ldr x11, [x4, #0xa70]\n   cmp x10, x11\n   b.hs 40f\n"
    "ldr x10, [x4, #0xa80]\n   cbz x10, 31f\n   ldr x10, [x4, #0xa90]\n"
    "cmp x2, x10\n   b.lo 40f\n"
    "31: mov w9, #1200\n   cmn w8, #1\n   b.eq 32f\n"
    "madd x10, x8, x9, x0\n   ldr w10, [x10, #20]\n   b 33f\n"
    "32: ldr w10, [x4, #0xa1c]\n"
    "33: cmn w10, #1\n   b.eq 40f\n   madd x11, x10, x9, x0\n"
    "ldrb w12, [x11, #34]\n   add x12, x4, x12, lsl #4\n   ldr w12, [x12, #0x204]\n"
    "cmn w12, #1\n   b.eq 34f\n"
    "madd x12, x12, x9, x0\n   ldr w13, [x11, #16]\n   ldr w12, [x12, #16]\n"
    "sub w13, w13, w12\n   cmp w13, #63\n   b.ls 34f\n"
    "mov w8, w10\n   ldr w10, [x11, #20]\n   b 33b\n"
    "34:\n"
    WATERLINK_A64_FRAME("0xa1c", "0xa24", "0xa2c", "40f", "30b",
                        "add x7, x7, x16\n")
    //  A probe spends one expiry's worth; otherwise the pacer charges what
    //  the normal band carried.
    "40: ldr w10, [x4, #0xab4]\n   cbz w10, 41f\n   cbz x7, 42f\n"
    "sub w10, w10, #1\n   str w10, [x4, #0xab4]\n"
    "42: mov x7, #0\n"
    "41: cbz x7, 100f\n   ldr x10, [x4, #0xa80]\n   cbz x10, 100f\n"
    "ldr x11, [x4, #0xa70]\n   add x11, x11, x11, lsl #2\n"
    "mul x12, x10, x7\n   lsl x12, x12, #2\n   udiv x12, x12, x11\n"
    "mov x13, #4800\n   mul x13, x10, x13\n   udiv x13, x13, x11\n   lsl x13, x13, #4\n"
    "subs x14, x2, x13\n   csel x14, x14, xzr, hi\n"
    "ldr x15, [x4, #0xa90]\n   cmp x15, x14\n   csel x15, x14, x15, lo\n"
    "add x15, x15, x12\n   str x15, [x4, #0xa90]\n"
    //  The acknowledgements, when frames go or they are due: x10 the keys
    //  owed, x16 and w17 the held pool, w11 the key, w12 how far it was
    //  taken and then its spelling, w13 its held chain, x14 the mask, x15
    //  the head, w9 the spelling's bytes.
    "100: ldr x10, [x4, #0xa10]\n   cbz x10, 119f\n   cbnz x5, 101f\n"
    "ldrb w11, [x4, #0xaac]\n   cbnz w11, 101f\n"
    "ldr w11, [x4, #0xaa8]\n   cmp w11, #2\n   b.hs 101f\n"
    "ldr x11, [x4, #0xaa0]\n   sub x11, x2, x11\n   cmp x11, #1000\n   b.lo 119f\n"
    "101: add x16, x0, #0x4b, lsl #12\n   mov w17, #1172\n"
    "102: rbit x11, x10\n   clz x11, x11\n   add x12, x11, x11, lsl #1\n"
    "add x12, x4, x12, lsl #2\n   ldr w13, [x12, #0x604]\n   ldr w12, [x12, #0x600]\n"
    "mov x14, #0\n   lsl w15, w11, #8\n   orr w15, w15, #8\n"
    "cmn w13, #1\n   b.eq 103f\n"
    "104: madd x6, x13, x17, x16\n   ldr w7, [x6]\n   ldr w13, [x6, #8]\n"
    "sub w7, w7, w12\n   sub w7, w7, #1\n   cmp w7, #63\n   b.hi 105f\n"
    "mov x8, #1\n   lsl x8, x8, x7\n   orr x14, x14, x8\n"
    "105: cmn w13, #1\n   b.ne 104b\n"
    "103: mov w9, #1\n   cmp w12, #127\n   b.hi 106f\n"
    "107: orr x15, x15, x12, lsl #16\n   cmp x14, #127\n   b.hi 108f\n"
    "add w6, w9, #2\n   lsl w6, w6, #3\n   lsl x14, x14, x6\n   orr x15, x15, x14\n"
    "add x6, x9, #3\n   add x7, x5, x6\n   cmp x7, #1168\n   b.hi 119f\n"
    "add x8, x1, x5\n   mov x5, x7\n   sub x7, x10, #1\n   and x10, x10, x7\n"
    "str x10, [x4, #0xa10]\n   cmp x6, #8\n   b.eq 109f\n"
    "str w15, [x8]\n   sub x6, x6, #4\n   lsl x7, x6, #3\n   lsr x15, x15, x7\n"
    "str w15, [x8, x6]\n   b 110f\n"
    "109: str x15, [x8]\n"
    "110: cbnz x10, 102b\n"
    "str wzr, [x4, #0xaa8]\n   strb wzr, [x4, #0xaac]\n"
    "119: mov x0, x5\n"
    ASM_RET
    "106: " WATERLINK_A64_SPELL("x12", "x6", "w6", "w7", "x9", "w9")
    "b 107b\n"
    //  A mask over seven bits: the head a byte at a time, then the mask.
    "108: clz x6, x14\n   eor x6, x6, #63\n   add x6, x6, x6, lsl #3\n"
    "add x6, x6, #73\n   lsr x6, x6, #6\n   add x6, x6, x9\n   add x6, x6, #2\n"
    "add x7, x5, x6\n   cmp x7, #1168\n   b.hi 119b\n"
    "add x8, x1, x5\n   mov x5, x7\n   sub x7, x10, #1\n   and x10, x10, x7\n"
    "str x10, [x4, #0xa10]\n   add w6, w9, #2\n"
    "111: strb w15, [x8], #1\n   lsr x15, x15, #8\n   subs w6, w6, #1\n   b.ne 111b\n"
    "112: cmp x14, #127\n   b.ls 113f\n   orr w6, w14, #0x80\n   strb w6, [x8], #1\n"
    "lsr x14, x14, #7\n   b 112b\n"
    "113: strb w14, [x8]\n   b 110b\n"
    ASM_END(waterlink_fill)
);
#elif RISCV64
/*
        One frame: t0 its slot, t1 the slot's address, t6 the one before it
        in the band. t2 the sequence, t3 the length, t4 the flags with the
        key over them, t5 the frame's bytes; s0, s1 and a3 go. The slot's
        fields are read first, its bookkeeping written next, and the frame
        last, a byte at a time: a body's bytes are wherever the frames
        before put them, and baseline riscv64 asks for aligned words.
*/
#define WATERLINK_RV_FRAME(head, tail, requeue, full, again, carried)          \
    "lwu t2, 16(t1)\n   lhu t3, 32(t1)\n   lbu t4, 34(t1)\n   lbu t5, 35(t1)\n" \
    "slli t4, t4, 8\n   or t4, t4, t5\n"                                       \
    "li s0, 128\n   addi t5, t3, 5\n   bgeu t3, s0, 51f\n   addi t5, t3, 4\n"   \
    "51: srli s1, t2, 7\n   beqz s1, 53f\n"                                    \
    "52: addi t5, t5, 1\n   srli s1, s1, 7\n   bnez s1, 52b\n"                  \
    "53: add s0, a5, t5\n   bltu a6, s0, 54f\n"                                \
    /*  Off the band, into the flight. */                                      \
    "55: lw s0, 20(t1)\n   bltz t6, 59f\n"                                     \
    "li s1, 1200\n   mul s1, t6, s1\n   add s1, a0, s1\n   sw s0, 20(s1)\n"    \
    "j 60f\n"                                                                  \
    "59: sw s0, " head "(a4)\n"                                                \
    "60: lw s0, " tail "(a4)\n   bne s0, t0, 61f\n   sw t6, " tail "(a4)\n"     \
    "61: lw s0, " requeue "(a4)\n   bne s0, t0, 62f\n"                         \
    "sw t6, " requeue "(a4)\n"                                                 \
    "62: ld s0, 848(a4)\n   addi s0, s0, 1\n   sd s0, 848(a4)\n"               \
    "sd s0, 8(t1)\n   sd a2, 0(t1)\n   li s0, 2\n   sb s0, 36(t1)\n"           \
    "lbu s0, 37(t1)\n   addi s1, s0, 1\n   sb s1, 37(t1)\n   bnez s0, 63f\n"   \
    "64: lw s0, 832(a4)\n   li s1, -1\n   sw s1, 20(t1)\n   sw s0, 24(t1)\n"   \
    "bltz s0, 65f\n"                                                           \
    "li s1, 1200\n   mul s1, s0, s1\n   add s1, a0, s1\n   sw t0, 20(s1)\n"    \
    "j 66f\n"                                                                  \
    "65: sw t0, 828(a4)\n"                                                     \
    "66: sw t0, 832(a4)\n   ld s0, 872(a4)\n   add s0, s0, t5\n   sd s0, 872(a4)\n" \
    "srli s0, t4, 8\n   slli s0, s0, 4\n   add s0, a4, s0\n"                   \
    "lhu s1, -1268(s0)\n   addi s1, s1, 1\n   sh s1, -1268(s0)\n"              \
    "ld s0, 992(a4)\n   addi s0, s0, 1\n   sd s0, 992(a4)\n"                   \
    /*  The frame. */                                                          \
    "add a3, a1, a5\n   add a5, a5, t5\n" carried                              \
    "sb t4, 0(a3)\n   srli s0, t4, 8\n   sb s0, 1(a3)\n   addi a3, a3, 2\n"     \
    "li s1, 128\n"                                                             \
    "70: bltu t2, s1, 71f\n   ori s0, t2, 128\n   sb s0, 0(a3)\n"              \
    "addi a3, a3, 1\n   srli t2, t2, 7\n   j 70b\n"                            \
    "71: sb t2, 0(a3)\n   addi a3, a3, 1\n   bltu t3, s1, 72f\n"                \
    "ori s0, t3, 128\n   sb s0, 0(a3)\n   srli s0, t3, 7\n   sb s0, 1(a3)\n"   \
    "addi a3, a3, 2\n   j 73f\n"                                               \
    "72: sb t3, 0(a3)\n   addi a3, a3, 1\n"                                    \
    "73: beqz t3, 67f\n   addi t1, t1, 38\n   li s0, 16\n   bltu s0, t3, 75f\n" \
    "add s0, t1, t3\n"                                                         \
    "74: lbu s1, 0(t1)\n   sb s1, 0(a3)\n   addi t1, t1, 1\n   addi a3, a3, 1\n" \
    "bne t1, s0, 74b\n"                                                        \
    "67: j " again "\n"                                                        \
    "75: addi sp, sp, -64\n   sd a0, 0(sp)\n   sd a1, 8(sp)\n   sd a2, 16(sp)\n" \
    "sd a4, 24(sp)\n   sd a5, 32(sp)\n   sd a6, 40(sp)\n   sd a7, 48(sp)\n"     \
    "sd t6, 56(sp)\n   mv a0, a3\n   mv a1, t1\n   mv a2, t3\n"                \
    "call memory_copy_apart\n"                                                 \
    "ld a0, 0(sp)\n   ld a1, 8(sp)\n   ld a2, 16(sp)\n   ld a4, 24(sp)\n"       \
    "ld a5, 32(sp)\n   ld a6, 40(sp)\n   ld a7, 48(sp)\n   ld t6, 56(sp)\n"     \
    "addi sp, sp, 64\n   j 67b\n"                                              \
    "54: bnez a5, " full "\n   li s0, 1168\n   bltu s0, t5, " full "\n"        \
    "li a6, 1168\n   j 55b\n"                                                  \
    "63: ld s0, 1000(a4)\n   addi s0, s0, 1\n   sd s0, 1000(a4)\n   j 64b\n"

/*
        a0 the link, a1 the body, a2 now, a3 where alone goes, a4 the link
        plus 0x6ff00 so every field past the slots is an offset from it, a5
        the bytes used, a6 the room, a7 the bytes the normal band carried,
        t6 a band's prior. With a frame to send, ra, s0, s1 and where alone
        goes are kept on the stack, and the losses walk's arguments across
        it. The key of an acknowledgement is its bit's place, which the
        double its power of two converts to says in its exponent.
*/
__asm__(
    ASM_FUNC(waterlink_fill)
    "sb zero, 0(a3)\n   lui a4, 0x70\n   addi a4, a4, -256\n   add a4, a0, a4\n"
    "ld t0, 840(a4)\n   bgeu t0, a2, 1f\n   sd a2, 840(a4)\n"
    //  Nothing in flight and nothing queued: the acknowledgements alone.
    "1:  lw t0, 828(a4)\n   lw t1, 792(a4)\n   lw t2, 796(a4)\n   and t1, t1, t2\n"
    "and t1, t1, t0\n   li a5, 0\n   addi t1, t1, 1\n   beqz t1, 100f\n"
    "addi sp, sp, -64\n   sd ra, 24(sp)\n   sd s0, 16(sp)\n   sd s1, 8(sp)\n"
    "sd a3, 0(sp)\n   bltz t0, 5f\n"
    "sd a0, 48(sp)\n   sd a1, 32(sp)\n   sd a2, 40(sp)\n   mv a1, a2\n"
    "call waterlink_losses\n"
    "ld a0, 48(sp)\n   ld a1, 32(sp)\n   ld a2, 40(sp)\n"
    "lui a4, 0x70\n   addi a4, a4, -256\n   add a4, a0, a4\n"
    //  Room kept for the acknowledgements owed, up to eight.
    "5:  li a5, 0\n   li a6, 1168\n   li a7, 0\n   ld t0, 784(a4)\n   beqz t0, 10f\n"
    "li t1, 0\n   li t2, 8\n"
    "6:  addi t1, t1, 1\n   addi t3, t0, -1\n   and t0, t0, t3\n   beqz t0, 7f\n"
    "bne t1, t2, 6b\n"
    "7:  slli t2, t1, 4\n   add t1, t1, t2\n   sub a6, a6, t1\n"
    //  The urgent band, from its head.
    "10: lw t0, 792(a4)\n   bltz t0, 20f\n"
    "li s0, 1200\n   mul t1, t0, s0\n   add t1, a0, t1\n   li t6, -1\n"
    WATERLINK_RV_FRAME("792", "800", "808", "19f", "10b", "")
    "19: ld t0, 0(sp)\n   li t1, 1\n   sb t1, 0(t0)\n   j 40f\n"
    "20: beqz a5, 21f\n   ld t0, 0(sp)\n   li t1, 1\n   sb t1, 0(t0)\n"
    "21: li t6, -1\n"
    //  The normal band, while the window and the pacer allow, passing over
    //  a key with its window out and going on from the prior.
    "30: lw t0, 948(a4)\n   bnez t0, 31f\n"
    "ld t0, 872(a4)\n   ld t1, 880(a4)\n   bgeu t0, t1, 40f\n"
    "ld t0, 896(a4)\n   beqz t0, 31f\n   ld t0, 912(a4)\n   bltu a2, t0, 40f\n"
    "31: li s0, 1200\n   bltz t6, 32f\n"
    "mul t0, t6, s0\n   add t0, a0, t0\n   lw t0, 20(t0)\n   j 33f\n"
    "32: lw t0, 796(a4)\n"
    "33: bltz t0, 40f\n   mul t1, t0, s0\n   add t1, a0, t1\n"
    "lbu t2, 34(t1)\n   slli t2, t2, 4\n   add t2, a4, t2\n   lw t2, -1276(t2)\n"
    "bltz t2, 34f\n"
    "mul t2, t2, s0\n   add t2, a0, t2\n   lwu t3, 16(t1)\n   lwu t2, 16(t2)\n"
    "subw t3, t3, t2\n   li t2, 64\n   bltu t3, t2, 34f\n"
    "mv t6, t0\n   lw t0, 20(t1)\n   j 33b\n"
    "34:\n"
    WATERLINK_RV_FRAME("796", "804", "812", "40f", "30b", "add a7, a7, t5\n")
    //  A probe spends one expiry's worth; otherwise the pacer charges what
    //  the normal band carried.
    "40: lw t0, 948(a4)\n   beqz t0, 41f\n   beqz a7, 42f\n"
    "addi t0, t0, -1\n   sw t0, 948(a4)\n"
    "42: li a7, 0\n"
    "41: beqz a7, 43f\n   ld t0, 896(a4)\n   beqz t0, 43f\n"
    "ld t1, 880(a4)\n   slli t2, t1, 2\n   add t1, t1, t2\n"
    "mul t2, t0, a7\n   slli t2, t2, 2\n   divu t2, t2, t1\n"
    "li t3, 4800\n   mul t3, t0, t3\n   divu t3, t3, t1\n   slli t3, t3, 4\n"
    "li t4, 0\n   bgeu t3, a2, 44f\n   sub t4, a2, t3\n"
    "44: ld t5, 912(a4)\n   bgeu t5, t4, 45f\n   mv t5, t4\n"
    "45: add t5, t5, t2\n   sd t5, 912(a4)\n"
    "43: ld ra, 24(sp)\n   ld s0, 16(sp)\n   ld s1, 8(sp)\n   addi sp, sp, 64\n"
    //  The acknowledgements, when frames go or they are due: t0 the keys
    //  owed, t5 and t6 the held pool, t1 the key, t3 how far it was taken,
    //  t2 its held chain, t4 the mask, a6 the bytes.
    "100: ld t0, 784(a4)\n   beqz t0, 119f\n   bnez a5, 101f\n"
    "lbu t1, 940(a4)\n   bnez t1, 101f\n"
    "lwu t1, 936(a4)\n   li t2, 2\n   bgeu t1, t2, 101f\n"
    "ld t1, 928(a4)\n   sub t1, a2, t1\n   li t2, 1000\n   bltu t1, t2, 119f\n"
    "101: li t5, 1172\n   lui t6, 0x4b\n   add t6, a0, t6\n"
    "102: neg t1, t0\n   and t1, t0, t1\n   fcvt.d.lu ft0, t1\n   fmv.x.d t1, ft0\n"
    "srli t1, t1, 52\n   addi t1, t1, -1023\n"
    "slli t2, t1, 1\n   add t2, t2, t1\n   slli t2, t2, 2\n   add t2, a4, t2\n"
    "lwu t3, -256(t2)\n   lw t2, -252(t2)\n   li t4, 0\n   bltz t2, 103f\n"
    "104: mul a3, t2, t5\n   add a3, t6, a3\n   lwu a6, 0(a3)\n   lw t2, 8(a3)\n"
    "subw a6, a6, t3\n   addiw a6, a6, -1\n   li a7, 64\n   bgeu a6, a7, 105f\n"
    "li a7, 1\n   sll a7, a7, a6\n   or t4, t4, a7\n"
    "105: bgez t2, 104b\n"
    "103: li a7, 128\n   li a6, 4\n   srli a3, t3, 7\n   beqz a3, 106f\n"
    "107: addi a6, a6, 1\n   srli a3, a3, 7\n   bnez a3, 107b\n"
    "106: srli a3, t4, 7\n   beqz a3, 108f\n"
    "109: addi a6, a6, 1\n   srli a3, a3, 7\n   bnez a3, 109b\n"
    "108: add a6, a5, a6\n   li a3, 1168\n   bltu a3, a6, 119f\n"
    "add a3, a1, a5\n   mv a5, a6\n   addi a6, t0, -1\n   and t0, t0, a6\n"
    "sd t0, 784(a4)\n   li a6, 8\n   sb a6, 0(a3)\n   sb t1, 1(a3)\n   addi a3, a3, 2\n"
    "110: bltu t3, a7, 111f\n   ori a6, t3, 128\n   sb a6, 0(a3)\n   addi a3, a3, 1\n"
    "srli t3, t3, 7\n   j 110b\n"
    "111: sb t3, 0(a3)\n   addi a3, a3, 1\n"
    "112: bltu t4, a7, 113f\n   ori a6, t4, 128\n   sb a6, 0(a3)\n   addi a3, a3, 1\n"
    "srli t4, t4, 7\n   j 112b\n"
    "113: sb t4, 0(a3)\n   bnez t0, 102b\n"
    "sw zero, 936(a4)\n   sb zero, 940(a4)\n"
    "119: mv a0, a5\n"
    ASM_RET
    ASM_END(waterlink_fill)
);
#endif

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
