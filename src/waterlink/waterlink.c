/*
        Waterlink: one authenticated link, many kinds of traffic.

        Not a remote shell. A shell is one thing that can ride this, beside a
        desktop, a log, a file, and whatever an application wants to send --
        and the link does not know which is which.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        WHY NOT SSH, AND WHY NOT TLS

        There are no users here, so SSH's authentication half answers a
        question this machine never asks: not "may alice log in" but "is this
        machine willing to talk to that operator, and what may they do". A
        static key pair answers it. TLS would bring certificates and a version
        matrix for a peer we already know by key. The handshake is Noise_IK
        over what lib.c already has -- X25519, HKDF, AES-GCM, SHA-256 -- as
        Noise_IK_25519_AES128GCM_SHA256: the standard's AESGCM is AES-256,
        which lib.c does not carry, and handshake.c says why the name had to
        change with it.

        Datagrams, not a stream, because a keystroke must not wait behind a
        video frame and one TCP connection cannot promise that: a lost segment
        stalls everything behind it. Head of line blocking is not a parameter,
        it is the protocol.

        THE KEY IS THE WHOLE DESIGN

        A frame carries a key, opaque to the link, doing two jobs at once: a
        newer frame for the same key replaces an older one not yet delivered,
        and frames for one key arrive in order while frames for different keys
        have no order between them. Those being one concept is what makes this
        small, and it lets an application pick its own granularity -- one key
        per screen, per entity, per window's damage.

        THE RULE THAT MAKES IT CORRECT

        The sender may drop a replaceable frame only if a newer frame for the
        same key is queued and no durable frame sits between them.

        That sentence is the scheduler. It throws away superseded work without
        letting an event overtake the state it describes.

        AND THE ONE THAT IS EASY TO GET WRONG

        Losing a superseded frame is correct; losing the last frame for a key
        is wrong forever, and losing a durable frame is wrong at once. So
        every frame is held by its sender until the far side acknowledges it,
        and the acknowledgement is per key and cumulative: "this key is
        delivered through sequence n, and of the ones after it I hold these,
        waiting for n + 1". A
        replaceable frame that is superseded while it waits is not sent
        again; its slot carries the current value instead, under a new
        sequence, so what is retransmitted is always the current value and
        never the old frame it already replaced. That is fixed size per slot,
        which is why the datapath allocates nothing.

        Per key and not per datagram, because the key is already the unit of
        order: one number per key says everything a receiver has, where a
        datagram's acknowledgement would still have to be turned back into
        which frames of which keys it carried. It is how QUIC acknowledges
        streams, without QUIC's packet ranges under it.

        THE RULE THAT MAKES RETRANSMISSION CORRECT

        A frame names the frame on its key it follows.

        follows is the sequence of the last frame on the key that must be
        delivered before this one: the last durable frame, or a replaceable
        frame that a durable one was posted behind -- which the supersession
        rule already says may not be dropped. A receiver delivers a frame
        once what it follows has been delivered, holds it back until then,
        and drops anything at or behind what the key has delivered. So a
        durable frame arrives exactly once and in order however the network
        reorders, loses or repeats datagrams, and a replaceable frame can
        still be skipped, because nothing follows it. The hold-back is a
        fixed pool; a frame that finds it full is dropped unacknowledged and
        comes again.

        A key's sequence counts from one and never wraps: a key carries at
        most 2^32 - 2 frames, and a sender that needs more uses a new key.

        LAST ends a key for good. The receiver remembers that it ended, so a
        copy of the last frame the sender repeats because the acknowledgement
        was lost is not taken for something new -- which is also why a key
        that has ended is never posted to again.

        WHAT A PEER CAN SPEND

        Every table here belongs to one link, and a link is one session with
        one peer: the queue, the hold-back, the key tables, the replay window.
        A peer that opens every key it can and never ends one fills its own
        session's table and stops its own traffic, and nobody else's. That is
        the trust boundary, stated rather than bounded per channel: an
        authenticated peer may waste the session it holds, not the machine.
        What keeps a peer from holding many sessions is the handshake's
        limit, not this file.

        A link is about 430 KB, taken once when the session is made and never
        grown: 300 KB of it the send slots, which are the window, 75 KB the
        hold-back, which is what exactly-once costs, and 40 KB the two key
        tables. That was 300 KB before the hold-back existed, and it is kept
        rather than cut, because a smaller queue is a smaller window and the
        window is the throughput on any path longer than a room.

        GROUPS: MACHINES THAT PAIR BY THEMSELVES

        Pairing by key is one person with two machines in front of them. A
        headless box installed somewhere nobody will stand is the other case,
        and for it a machine joins a group -- a namespace and a secret --
        and pairs by itself with every member it finds on the same local
        network (discover.c finds them, pair.c pairs them). The rules:

        The secret is the grant. Whoever holds it gets, on every member, what
        that member's join line granted, and the verbs when it granted
        nothing. Taking one machine out of a group is changing the secret on
        all the others, and forgetting the one that left; there is no list of
        members to strike a name from, only the secret.

        Nothing announced names the group, the machine or its key. What is on
        the network is that a waterlink machine is here, under labels that
        change every boot and every hour. Discovery never leaves the local
        link: mDNS on 224.0.0.251, believed only at TTL 255, which no router
        forwards. There is no rendezvous and no NAT traversal.

        A weak secret can be guessed offline by anyone who hears one
        announcement, so every key comes from the secret through
        WATERLINK_GROUP_ROUNDS of PBKDF2, a protocol constant, and a secret
        the machine makes itself has 160 random bits. The secret itself is
        not kept: /root/link.groups, root's alone, holds what was derived
        from it.

        A member met for the first time is paired by Noise XXpsk0 under a key
        from the group, which a machine without the secret cannot get past the
        first message of, and does not learn a static key from. From then on
        it is an ordinary peer -- IK, by the key it was given -- carrying the
        mark of the group that paired it, and auto-pairing never replaces or
        widens a record that is already there, by hand or by another group.

        A machine in no group announces nothing and answers no one.

        HOW MUCH MAY BE IN FLIGHT

        A sender must not put more in flight than the path carries, and must
        not send its window as one burst: it keeps an estimate of the round
        trip, counts a frame lost when frames sent after it have arrived or a
        timer runs out, gives the path less when frames are lost and more when
        they are not, and paces what it sends across the round trip. Which
        algorithm does that is the sender's own and never crosses the wire;
        link.c says which it uses. Acknowledgements and urgent frames are not
        held by it: the first are what open the window, and the second are a
        few bytes somebody is waiting to see.

        EVERY SETTING IS PER SEND

        There are no stream types and no stored per-channel settings. A caller
        names the packing and the urgency on each send, because a terminal
        carrying a file for a moment is the ordinary case, not the exception.
        Only the packing method crosses the wire, and only because unpacking
        needs it.

        This file is the contract and nothing else, included where lib.c's
        types are already in scope.
*/

#ifndef WATERLINK_INCLUDED
#define WATERLINK_INCLUDED

#define WATERLINK_MAGIC 0x4b4e4c57u // "WLNK", little endian
#define WATERLINK_VERSION 1

/*      1200 bytes clears the common 1280 minimum with room for an outer
        header and asks nothing of path discovery. A larger frame is the
        sender's to split: reassembly is a queue, and a queue is a place to
        store an attacker's bytes.

        Every datagram that carries a frame is padded to this, which buys two
        things and the second one is worth more. A packed frame's length leaks nothing to an
        observer. And segment offload will only cut a buffer into datagrams
        that are all the same size -- so the fixed size is what lets one send
        hand the kernel forty datagrams for the price of one. Measured on a
        9950X: 2516 cycles a datagram sent one at a time, 2355 batched through
        sendmmsg, 461 as uniform segments. Batching the syscall is worth six
        percent; batching the segments is worth five times.

        One that carries only acknowledgements is cut to whole blocks
        instead: it never rides in a segment run, and padded it made the
        return path as heavy as the forward one. */
#define WATERLINK_DATAGRAM 1200
#define WATERLINK_PAYLOAD (WATERLINK_DATAGRAM - 16 - 16)

/*      Read before any key is in hand, so it carries only what a receiver
        needs to find one. receiver is the index the far side handed out at
        handshake; counter is the nonce and the replay sequence both. */
struct waterlink_datagram {
        unsigned int kind;     // WATERLINK_KIND_*
        unsigned int receiver;
        unsigned long counter;
};

_Static_assert(sizeof(struct waterlink_datagram) == 16,
               "waterlink datagram header must be exactly 16 bytes");

#define WATERLINK_KIND_INITIATE 1u // Noise_IK message one
#define WATERLINK_KIND_RESPOND 2u  // Noise_IK message two
#define WATERLINK_KIND_CARRY 3u    // frames, once the session is up
#define WATERLINK_KIND_CLOSE 4u

/*      A frame, inside the box, so the code that parses attacker-shaped bytes
        only ever runs on bytes that were already authenticated.

        deadline is milliseconds from the sender's own clock reading, not a
        timestamp: the ends never agree on what time it is, only on how long a
        thing is worth waiting for. inflated is what the payload unpacks to,
        declared so the receiver can refuse it before unpacking anything.
        follows is the rule above; zero for a key's first frame, or for a
        frame nothing on its key has to come before.

        Packed, because frames sit back to back in the box at any offset and
        are only ever copied in and out whole. */
struct waterlink_frame {
        unsigned long key;     // opaque to the link; the sender's meaning
        unsigned int sequence; // per key, from one, increasing
        unsigned int follows;  // the sequence this frame is delivered after
        unsigned short channel;
        unsigned short length;   // packed bytes following this header
        unsigned short deadline; // milliseconds, 0 for none
        unsigned short flags;    // WATERLINK_FRAME_*
        unsigned short inflated; // 0 when not packed
        unsigned short reserved; // must be 0
} __attribute__((packed));

#define WATERLINK_HEADER 28

_Static_assert(sizeof(struct waterlink_frame) == WATERLINK_HEADER,
               "waterlink frame header must be exactly 28 bytes");

// A frame with neither of these is refused rather than guessed at.
#define WATERLINK_FRAME_REPLACEABLE 0x0001u
#define WATERLINK_FRAME_DURABLE 0x0002u
#define WATERLINK_FRAME_LAST 0x0004u // the key ends with this frame

/*      The link's own frame, and the only one with neither class: records
        of twenty four bytes -- key, delivered through, highest seen, and a
        mask of which of the sixty four sequences after the first missing one
        are held -- with every other header field zero. A caller cannot post
        one. */
#define WATERLINK_FRAME_ACK 0x0200u

/*      Urgency, and only the tiebreak: deadline says when a frame is worth
        sending, this says which one goes first when two are both about to
        miss. Without it a desktop that is always late starves a keystroke
        that is merely late. */
#define WATERLINK_FRAME_URGENT 0x0040u // a keystroke; nothing waits behind it
#define WATERLINK_FRAME_BULK 0x0080u   // a file; throughput, not latency

/*      Which is why sending is two paths and not one. Segments only go out
        in a run, and a run is built by waiting for the next frame -- so an
        urgent frame leaves alone, immediately, at the unbatched price, and
        bulk accumulates into a run for one peer at one size. A scheduler that
        put a keystroke in a segment run would be holding it for forty frames
        it has nothing to do with, which is the thing this link exists to
        refuse. The price of being right here is known and small: an urgent
        frame costs 2516 cycles where a bulk one costs 461. */

/*      How the payload is packed. Three bits, so a method is a small number
        and not a negotiation: a receiver that does not know one refuses the
        frame, and a sender only packs with what the version says the far side
        has. A list would be a negotiation, and a negotiation is a downgrade.

        THE RULE THAT MAKES PACKING CORRECT

        A packed replaceable frame must be self contained.

        These coders earn their ratio from history, but a replaceable frame
        may be dropped -- that is what supersession is for -- and a dropped
        frame takes the decoder's window with it, so every later frame on that
        key unpacks into garbage the tag still calls authentic. History across
        frames is therefore legal only where every frame is durable, and the
        sender refuses the first replaceable frame that would break it.

        Which says what packing is worth: 1168 bytes with a fresh window is
        not much of a corpus. It pays on files, on the log, and on a terminal's
        own output, which is mostly spaces and repeated escapes. It does not
        pay on a desktop frame, already coded, or on a keystroke. */
#define WATERLINK_FRAME_PACK_SHIFT 3
#define WATERLINK_FRAME_PACK_MASK 0x0038u
#define WATERLINK_FRAME_HISTORY 0x0100u // durable-only streams; see above

#define WATERLINK_PACK_NONE 0u
#define WATERLINK_PACK_DEFLATE 1u
#define WATERLINK_PACK_ZSTD 2u
#define WATERLINK_PACK_LZMA 3u

/*      What a frame may unpack to: one scratch buffer taken at startup, and
        nothing may claim more. A paired peer is authenticated, not benign, so
        without a ceiling the datapath allocates nothing only until somebody
        sends a well formed frame claiming a gigabyte.

        The ceiling is the width of the field that declares it. Sixteen bits
        cannot ask for more than this, so there is no bound to check and no
        check to get wrong -- the only such rule in this file that a reader
        can confirm by looking at the struct. */
#define WATERLINK_INFLATED 65535

/*      Effort is a level on the method's own scale, not a time budget: no
        coder here takes microseconds as input, and a cost table per method
        per architecture does not exist. The deadline does that work instead
        -- when it is closer than packing last cost, send the frame raw. */
#define WATERLINK_EFFORT_CHEAP 1
#define WATERLINK_EFFORT_HARD 6

/*      What a peer may do, granted one at a time and starting at none. VERBS
        is the moonwater vocabulary the machine already answers to and is what
        a freshly paired peer gets: it cannot execute, open a terminal or
        write a file. Everything past it is a decision somebody made. */
#define WATERLINK_MAY_VERBS 0x0001u
#define WATERLINK_MAY_RUN 0x0002u      // one command, no terminal
#define WATERLINK_MAY_SHELL 0x0004u    // a terminal
#define WATERLINK_MAY_SCREEN 0x0008u   // watch the desktop
#define WATERLINK_MAY_FILES 0x0010u    // push and pull
#define WATERLINK_MAY_LOG 0x0020u      // follow the kernel log
#define WATERLINK_MAY_CHANNELS 0x0040u // open channels of its own

#define WATERLINK_MAY_DEFAULT WATERLINK_MAY_VERBS

#define WATERLINK_CHANNEL_CONTROL 0u // verbs, and their answers
#define WATERLINK_CHANNEL_SHELL 1u
#define WATERLINK_CHANNEL_SCREEN 2u
#define WATERLINK_CHANNEL_LOG 3u
#define WATERLINK_CHANNEL_FILES 4u
#define WATERLINK_CHANNEL_OPEN 16u // the first an application may take

/*      AES-128-GCM, and not 256: it is the key size lib.c carries in assembly
        on all three machines, with a bitsliced floor that a kernel build
        keeps. With no cipher agility the choice is made once, and it is made
        for the one that is fast everywhere this link runs. */
#define WATERLINK_KEY_BYTES 32  // X25519
#define WATERLINK_AEAD_BYTES 16 // AES-128
#define WATERLINK_TAG_BYTES 16
#define WATERLINK_NAME_MAX 32

/*      The datagram is whole AES blocks, and that is chosen, not found: the
        cleartext header is one block and is exactly GCM's associated data,
        the box is seventy three, and the tag is one. So sealing a datagram is
        one counter-mode call over header-and-box and one GHASH call over all
        seventy five, the lengths block borrowing the tag's slot -- no partial
        block anywhere, and no tail for any of the assembly under it. */
_Static_assert(WATERLINK_DATAGRAM % 16 == 0 && WATERLINK_PAYLOAD % 16 == 0,
               "a waterlink datagram must be whole AES blocks");
_Static_assert(WATERLINK_DATAGRAM == 16 + WATERLINK_PAYLOAD + WATERLINK_TAG_BYTES,
               "header, box and tag must be the whole datagram");

/*      A peer, as this machine keeps it -- the far side never sees this
        record. An address is a cache and not an identity: a machine that
        moves keeps its key and loses its address, which is the ordinary case
        on wifi and the reason a name binds to a key rather than a place. This
        goes in the image's settings block beside the wireless networks, so it
        survives an install the way those do. */
struct waterlink_peer {
        unsigned char key[WATERLINK_KEY_BYTES];
        char name[WATERLINK_NAME_MAX];
        unsigned int may;   // WATERLINK_MAY_*
        unsigned int group; // 0 paired by hand, or the mark of the group
                            // that paired it (discover.c)
        unsigned char address[16]; // last seen, v6 or v4 mapped
        unsigned short port;
        unsigned short address_flags;
        unsigned int seen; // seconds, this machine's clock, 0 for never
};

_Static_assert(sizeof(struct waterlink_peer) == 96,
               "waterlink peer record must be exactly 96 bytes");

/*      Rekeying is a correctness rule and not a policy: the counter is the
        nonce, so a session ends long before it could repeat one. WireGuard's
        numbers, for WireGuard's reason -- whichever comes first. */
#define WATERLINK_REKEY_MESSAGES (1UL << 40)
#define WATERLINK_REKEY_SECONDS 120
#define WATERLINK_REJECT_SECONDS 180

// How far behind the highest counter a datagram may arrive and still be new.
#define WATERLINK_REPLAY_WINDOW 2048

#endif // WATERLINK_INCLUDED
