/*
        Waterlink's core, as a transform.

        Frames in, datagram bodies out, and the same in reverse. Nothing here
        opens a socket, reads a clock, takes memory or touches a key: the
        caller passes the time it already read and the buffer it already owns,
        and gets bytes back. That is not tidiness. It is what lets the same
        code be the kernel's datapath on Moonwater and a userspace socket loop
        on a machine that is not Moonwater, and it is what lets the scheduler
        and the replay window be tested to the end without a network.

        The rules this file implements are stated in waterlink.c and are not
        restated here. What is here is how they are kept.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_LINK_INCLUDED
#define WATERLINK_LINK_INCLUDED

#include "waterlink.c"

// The largest frame that can share a datagram with nothing else.
#define WATERLINK_FRAME_MAX (WATERLINK_PAYLOAD - 24)

/*
        Ceilings, not guesses at a working set. A fixed count is what buys no
        allocation failure on the datapath and a cost that does not depend on
        what the far side does. If one is ever too low, raise it here -- that
        is the whole intent of naming it.
*/
#define WATERLINK_SLOTS 256 // frames that may be queued at once
#define WATERLINK_KEYS 512  // live keys tracked in each direction

#define WATERLINK_NONE 0xffffffffu

/*
        Three queues, because urgency is a band and not a sort.

        Sorting by deadline would be the obvious thing and is the wrong thing:
        it puts a scan on the send path, and within a band post order is
        already the order the ordering rule demands. So a band is a list, a
        frame joins the end of its band's list, and supersession replaces a
        frame where it stands rather than moving it. Position is meaning here.
*/
#define WATERLINK_BAND_URGENT 0
#define WATERLINK_BAND_TIMED 1
#define WATERLINK_BAND_BULK 2
#define WATERLINK_BANDS 3

struct waterlink_slot {
        p64 key;
        p32 sequence;
        p32 posted; // sender milliseconds, for the deadline only
        p32 next;   // the next slot in this band, or WATERLINK_NONE
        p16 channel;
        p16 length;
        p16 deadline;
        p16 flags;
        p16 inflated;
        p8 payload[WATERLINK_FRAME_MAX];
};

/*
        One live key, in one direction.

        Sending, it holds the next sequence to assign and the slot of the last
        frame still queued for this key -- which is the only thing supersession
        needs to ask about. Receiving, it holds the highest sequence handed to
        the application, which is how a frame that arrives after the frame that
        replaced it gets dropped rather than applied backwards.
*/
struct waterlink_live {
        p64 key;
        p32 sequence;
        p32 pending; // sending: slot index, or WATERLINK_NONE
        p8 taken;
        p8 started; // receiving: whether sequence means anything yet
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
        struct waterlink_live sending[WATERLINK_KEYS];
        struct waterlink_live receiving[WATERLINK_KEYS];
        struct waterlink_replay replay;

        p32 head[WATERLINK_BANDS];
        p32 tail[WATERLINK_BANDS];
        p32 free;

        // What the caller may want to know without instrumenting the caller.
        p64 posted;
        p64 superseded;
        p64 expired;
        p64 refused;
        p64 delivered;
        p64 stale;
};

/*
        Key tables are open addressed with linear probing and are never
        deleted from in place -- a key leaves only when the link is reset or
        the frame flagged LAST retires it, and retiring clears the entry and
        walks the run behind it back, which is the one deletion a linear probe
        table can do without tombstones.
*/
static p32 waterlink_key_slot(struct waterlink_live address_to table, p64 key,
                              bool make)
{
        p32 at = (p32)((key * 0x9e3779b97f4a7c15ull) >> 55) &
                 (WATERLINK_KEYS - 1);

        for (p32 step = 0; step < WATERLINK_KEYS; step++)
        {
                struct waterlink_live address_to live = table + at;

                if (live->taken && live->key == key)
                        return at;

                if (!live->taken)
                {
                        if (!make)
                                return WATERLINK_NONE;

                        live->key = key;
                        live->sequence = 0;
                        live->pending = WATERLINK_NONE;
                        live->started = 0;
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
                p32 wanted = (p32)((table[scan].key * 0x9e3779b97f4a7c15ull) >>
                                   55) &
                             (WATERLINK_KEYS - 1);
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

        link->free = 0;

        for (p32 band = 0; band < WATERLINK_BANDS; band++)
        {
                link->head[band] = WATERLINK_NONE;
                link->tail[band] = WATERLINK_NONE;
        }
}

static p32 waterlink_band_of(p16 flags)
{
        if (flags & WATERLINK_FRAME_URGENT)
                return WATERLINK_BAND_URGENT;
        if (flags & WATERLINK_FRAME_BULK)
                return WATERLINK_BAND_BULK;
        return WATERLINK_BAND_TIMED;
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
*/
static bool waterlink_frame_sane(p16 flags, p16 length, p16 inflated)
{
        bool replaceable = (flags & WATERLINK_FRAME_REPLACEABLE) != 0;
        bool durable = (flags & WATERLINK_FRAME_DURABLE) != 0;

        if (replaceable == durable)
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

/*
        Queue a frame for sending.

        The supersession rule, in the one place it lives: a replaceable frame
        whose key already has a replaceable frame queued takes that frame's
        place, keeping its position in the band so nothing it describes can be
        overtaken. A durable frame always joins the end, and a replaceable
        frame behind a durable one joins the end too -- because the durable
        frame is the thing the rule says must not be jumped.

        Returns false and counts a refusal when the frame is malformed or the
        queue is full. A full queue is the caller's signal to stop producing,
        not this file's to start choosing.
*/
bool waterlink_post(struct waterlink_link address_to link, p64 key, p16 channel,
                    p16 flags, p16 deadline, p16 inflated,
                    address_any payload, p16 length, p32 now)
{
        struct waterlink_slot address_to slot;
        struct waterlink_live address_to live;
        p32 key_at;
        p32 band;
        p32 at;

        if (!waterlink_frame_sane(flags, length, inflated))
        {
                link->refused++;
                return false;
        }

        key_at = waterlink_key_slot(link->sending, key, true);
        if (key_at == WATERLINK_NONE)
        {
                link->refused++;
                return false;
        }

        live = link->sending + key_at;
        band = waterlink_band_of(flags);

        //      Supersession: the standing frame is replaced where it lies.
        if ((flags & WATERLINK_FRAME_REPLACEABLE) &&
            live->pending != WATERLINK_NONE &&
            (link->slot[live->pending].flags & WATERLINK_FRAME_REPLACEABLE) &&
            waterlink_band_of(link->slot[live->pending].flags) == band)
        {
                slot = link->slot + live->pending;
                slot->sequence = live->sequence++;
                slot->posted = now;
                slot->channel = channel;
                slot->length = length;
                slot->deadline = deadline;
                slot->flags = flags;
                slot->inflated = inflated;
                if (length)
                        memory_copy(slot->payload, payload, length);
                link->superseded++;
                return true;
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
        slot->posted = now;
        slot->next = WATERLINK_NONE;
        slot->channel = channel;
        slot->length = length;
        slot->deadline = deadline;
        slot->flags = flags;
        slot->inflated = inflated;
        if (length)
                memory_copy(slot->payload, payload, length);

        if (link->tail[band] == WATERLINK_NONE)
                link->head[band] = at;
        else
                link->slot[link->tail[band]].next = at;
        link->tail[band] = at;

        live->pending = at;
        link->posted++;
        return true;
}

static fn waterlink_slot_free(struct waterlink_link address_to link, p32 at)
{
        struct waterlink_live address_to live;
        p32 key_at = waterlink_key_slot(link->sending, link->slot[at].key,
                                        false);

        if (key_at != WATERLINK_NONE)
        {
                live = link->sending + key_at;
                if (live->pending == at)
                        live->pending = WATERLINK_NONE;
                if (link->slot[at].flags & WATERLINK_FRAME_LAST)
                        waterlink_key_retire(link->sending, key_at);
        }

        link->slot[at].next = link->free;
        link->free = at;
}

/*
        Take the next frame worth sending from a band, dropping what has run
        out of time on the way.

        A deadline that has passed makes a replaceable frame worthless, so it
        goes; a durable one is never dropped, because losing the last frame
        for a key is wrong forever and a late truth still beats no truth.
*/
static p32 waterlink_band_take(struct waterlink_link address_to link, p32 band,
                               p32 now)
{
        while (link->head[band] != WATERLINK_NONE)
        {
                p32 at = link->head[band];
                struct waterlink_slot address_to slot = link->slot + at;
                bool late = slot->deadline &&
                            (p32)(now - slot->posted) > slot->deadline;

                if (!late || (slot->flags & WATERLINK_FRAME_DURABLE))
                        return at;

                link->head[band] = slot->next;
                if (link->head[band] == WATERLINK_NONE)
                        link->tail[band] = WATERLINK_NONE;

                waterlink_slot_free(link, at);
                link->expired++;
        }

        return WATERLINK_NONE;
}

static fn waterlink_band_drop(struct waterlink_link address_to link, p32 band)
{
        p32 at = link->head[band];

        link->head[band] = link->slot[at].next;
        if (link->head[band] == WATERLINK_NONE)
                link->tail[band] = WATERLINK_NONE;

        waterlink_slot_free(link, at);
}

/*
        Fill one datagram body with as many whole frames as fit.

        Sets alone when the body holds an urgent frame. An urgent frame cannot
        wait in a segment run for the forty frames behind it -- a run is built
        by waiting, and waiting is the thing this link exists to refuse -- so
        the caller sends that datagram by itself and pays the unbatched price
        on purpose. Everything else accumulates.

        Frames already queued still ride along behind the urgent one. They
        were not going to leave sooner in any case, the datagram is padded to
        a fixed size whether they are in it or not, and the urgent frame is in
        front of them: the ride is free and it delays nothing.

        Returns the bytes written, or zero when there is nothing to send.
*/
positive waterlink_fill(struct waterlink_link address_to link,
                        address_any out, p32 now, bool address_to alone)
{
        p8 address_to bytes = (p8 address_to)out;
        positive used = 0;

        address_to alone = false;

        for (p32 band = 0; band < WATERLINK_BANDS; band++)
        {
                for (;;)
                {
                        struct waterlink_frame head;
                        struct waterlink_slot address_to slot;
                        p32 at = waterlink_band_take(link, band, now);

                        if (at == WATERLINK_NONE)
                                break;

                        slot = link->slot + at;
                        if (used + 24 + slot->length > WATERLINK_PAYLOAD)
                                break;

                        head.key = slot->key;
                        head.sequence = slot->sequence;
                        head.channel = slot->channel;
                        head.length = slot->length;
                        head.deadline = slot->deadline;
                        head.flags = slot->flags;
                        head.inflated = slot->inflated;
                        head.reserved = 0;

                        memory_copy(bytes + used, address_of head, 24);
                        used += 24;
                        if (slot->length)
                                memory_copy(bytes + used, slot->payload,
                                            slot->length);
                        used += slot->length;

                        if (band == WATERLINK_BAND_URGENT)
                                address_to alone = true;

                        waterlink_band_drop(link, band);
                }

                //      A band that could not fit its next frame has not
                //      finished, and a later band must not jump it.
                if (link->head[band] != WATERLINK_NONE)
                        break;
        }

        return used;
}

/*
        Walk an authenticated body and hand its frames to the application.

        Every bound is checked against what the body says it is, and nothing
        here trusts a length twice. A frame whose sequence is not past what
        this key has already delivered is a frame that arrived behind the one
        that replaced it: dropping it is the receiving half of supersession,
        and applying it would be reading a window's damage after the window
        closed.

        Returns false when the body is malformed, which for an authenticated
        peer means a bug on the far side rather than an attack -- and is still
        a refusal, because a link that guesses is a link with two opinions.
*/
typedef fn (address_to waterlink_sink)(address_any context,
                                       struct waterlink_frame address_to head,
                                       p8 address_to payload);

bool waterlink_deliver(struct waterlink_link address_to link,
                       address_any body, positive length,
                       waterlink_sink sink, address_any context)
{
        p8 address_to bytes = (p8 address_to)body;
        positive at = 0;

        if (length > WATERLINK_PAYLOAD)
                return false;

        while (at + 24 <= length)
        {
                struct waterlink_frame head;
                struct waterlink_live address_to live;
                p32 key_at;

                memory_copy(address_of head, bytes + at, 24);
                at += 24;

                if (head.reserved)
                        return false;

                if (!waterlink_frame_sane(head.flags, head.length,
                                          head.inflated))
                        return false;

                if (at + head.length > length)
                        return false;

                key_at = waterlink_key_slot(link->receiving, head.key, true);
                if (key_at == WATERLINK_NONE)
                        return false;

                live = link->receiving + key_at;

                //      Sequence counts from zero, so a fresh entry cannot say
                //      "nothing yet" with a value. It says it with started.
                if (live->started && head.sequence <= live->sequence)
                {
                        link->stale++;
                        at += head.length;
                        continue;
                }

                live->sequence = head.sequence;
                live->started = 1;

                if (sink)
                        sink(context, address_of head, bytes + at);

                at += head.length;
                link->delivered++;

                if (head.flags & WATERLINK_FRAME_LAST)
                        waterlink_key_retire(link->receiving, key_at);
        }

        return at == length;
}

/*
        Whether a datagram's counter is one this session has not seen.

        Accepting marks it, so this is asked once per datagram and only after
        the tag has verified -- a counter taken from an unauthenticated header
        would let anyone at all slide the window forward and lock the session
        out of its own traffic.
*/
bool waterlink_replay_new(struct waterlink_replay address_to window, p64 counter)
{
        positive words = WATERLINK_REPLAY_WINDOW / 64;
        p64 behind;

        if (counter > window->top)
        {
                p64 step = counter - window->top;

                if (step >= WATERLINK_REPLAY_WINDOW)
                        memory_zero(window->seen, sizeof(window->seen));
                else
                {
                        p64 whole = step / 64;
                        p64 part = step % 64;

                        for (positive i = words; i-- > 0;)
                        {
                                p64 high = i >= whole
                                                   ? window->seen[i - whole]
                                                   : 0;
                                p64 low = part && i > whole
                                                  ? window->seen[i - whole - 1]
                                                  : 0;

                                window->seen[i] = part
                                                          ? (high << part) |
                                                                    (low >>
                                                                     (64 - part))
                                                          : high;
                        }
                }

                window->top = counter;
                window->seen[0] |= 1;
                return true;
        }

        behind = window->top - counter;
        if (behind >= WATERLINK_REPLAY_WINDOW)
                return false;

        if (window->seen[behind / 64] & (1ull << (behind % 64)))
                return false;

        window->seen[behind / 64] |= 1ull << (behind % 64);
        return true;
}

#endif // WATERLINK_LINK_INCLUDED
