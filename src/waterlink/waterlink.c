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
        over what lib.c already has -- X25519, HKDF, AES-GCM, SHA-256.

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
        is wrong forever. A sender keeps, per key, what the far side last
        acknowledged and what it has now, and retransmits the current value --
        never the old frame it already replaced. That is fixed size per key,
        which is why the datapath allocates nothing.

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

        Every datagram is padded to this, which buys two things and the second
        one is worth more. A packed frame's length leaks nothing to an
        observer. And segment offload will only cut a buffer into datagrams
        that are all the same size -- so the fixed size is what lets one send
        hand the kernel forty datagrams for the price of one. Measured on a
        9950X: 2516 cycles a datagram sent one at a time, 2355 batched through
        sendmmsg, 461 as uniform segments. Batching the syscall is worth six
        percent; batching the segments is worth five times. */
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
        declared so the receiver can refuse it before unpacking anything. */
struct waterlink_frame {
        unsigned long key;     // opaque to the link; the sender's meaning
        unsigned int sequence; // per key, increasing
        unsigned short channel;
        unsigned short length;   // packed bytes following this header
        unsigned short deadline; // milliseconds, 0 for none
        unsigned short flags;    // WATERLINK_FRAME_*
        unsigned short inflated; // 0 when not packed
        unsigned short reserved; // must be 0
};

_Static_assert(sizeof(struct waterlink_frame) == 24,
               "waterlink frame header must be exactly 24 bytes");

// A frame with neither of these is refused rather than guessed at.
#define WATERLINK_FRAME_REPLACEABLE 0x0001u
#define WATERLINK_FRAME_DURABLE 0x0002u
#define WATERLINK_FRAME_LAST 0x0004u // the receiver may forget the key after it

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

#define WATERLINK_KEY_BYTES 32 // X25519, and the AEAD key
#define WATERLINK_TAG_BYTES 16 // AES-GCM
#define WATERLINK_NAME_MAX 32

/*      A peer, as this machine keeps it -- the far side never sees this
        record. An address is a cache and not an identity: a machine that
        moves keeps its key and loses its address, which is the ordinary case
        on wifi and the reason a name binds to a key rather than a place. This
        goes in the image's settings block beside the wireless networks, so it
        survives an install the way those do. */
struct waterlink_peer {
        unsigned char key[WATERLINK_KEY_BYTES];
        char name[WATERLINK_NAME_MAX];
        unsigned int may;      // WATERLINK_MAY_*
        unsigned int reserved; // must be 0
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
