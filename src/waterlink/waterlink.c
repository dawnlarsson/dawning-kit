/*
        Waterlink: one authenticated link, many kinds of traffic.

        Not a remote shell. A shell is one thing that can ride this, beside a
        desktop, a log, a file, and whatever an application wants to send --
        and the link does not know which is which.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        WHY THIS IS NOT SSH, AND NOT TLS

        SSH bundles a transport, a user database, channel multiplexing, a
        pseudoterminal, port forwarding and a file protocol. Most of that is
        1995 answering questions this machine does not ask. There are no
        users here -- no PAM, no /etc/shadow -- so the whole authentication
        half is a category error: the question was never "may alice log in"
        but "is this machine willing to talk to that operator, and what may
        they do". A static key pair answers it.

        Channel multiplexing existed because a TCP connection plus an
        authentication round trip was expensive and one wanted many streams
        for the price. That is not the reason to multiplex now. The reason is
        that a keystroke must not wait behind a video frame, and one TCP
        stream cannot promise that at all: a lost segment stalls everything
        behind it. Head of line blocking is not a parameter, it is the
        protocol. So this is datagrams.

        TLS would bring certificates, negotiation and a version matrix for a
        peer we already know by key. The handshake here is Noise_IK over the
        primitives already in lib.c -- X25519, HKDF, AES-GCM, SHA-256 -- and
        nothing else. No certificate chain, no cipher agility, no downgrade
        surface, and the two sides are near symmetric, where a TLS server is
        far larger than the TLS client this tree already has.

        THE KEY IS THE WHOLE DESIGN

        A frame carries a key. The key is opaque to the link -- what it means
        is the sender's business -- and it does two jobs at once:

            supersession    a newer frame for the same key replaces an older
                            one that has not been delivered
            ordering        frames for one key arrive in order; frames for
                            different keys have no order between them

        Those being the same thing is what makes this small. No head of line
        blocking falls out of the same concept that gives latest-wins, and an
        application picks its own granularity: one key for a whole screen,
        one key per entity, one key per window's damage.

        A frame is replaceable or durable, and that is per frame, not per
        channel. A terminal is screen state until someone sends a file
        through it. A desktop is timed frames while it animates and one
        last durable frame when it stops -- because a dropped final frame is
        a screen that stays wrong forever.

        THE RULE THAT MAKES IT CORRECT

        The sender may drop a replaceable frame only if a newer frame for the
        same key is queued and no durable frame sits between them.

        That single sentence is the scheduler. It throws away superseded work
        without ever letting an event overtake the state it describes, so
        "the window closed" cannot be applied before that window's last
        damage.

        AND THE ONE THAT IS EASY TO GET WRONG

        State is reliable in the limit and never retransmits a stale value.
        Losing a superseded frame is correct; losing the last frame for a key
        is wrong forever. So a sender keeps, per key, the value the far side
        last acknowledged and the value it has now, and retransmits the
        current value against that acknowledgement -- never the old frame it
        already replaced.

        That is bounded and fixed size per key, which is why the datapath
        allocates nothing: a ring of packet buffers and a small table of live
        keys, taken once.

        WHAT IS OUTSIDE AND WHAT IS INSIDE

        A datagram is a short cleartext header and then one AEAD box. The
        header carries only what a receiver needs before it holds a key: who
        the datagram is for, and the counter that makes the nonce. Everything
        with structure -- channels, keys, lengths -- is inside the box, so
        the code that parses attacker-shaped bytes only ever runs on bytes
        that were already authenticated.

        This file is the contract and nothing else: the numbers, the layouts
        and the rules both ends agree on. It is included where lib.c's types
        are already in scope.
*/

#ifndef WATERLINK_INCLUDED
#define WATERLINK_INCLUDED

// "WLNK", little endian
#define WATERLINK_MAGIC 0x4b4e4c57u

#define WATERLINK_VERSION 1

/*
        The datagram ceiling.

        1200 bytes is what fits through the paths that exist rather than the
        paths that should: it clears the common 1280 minimum with room for an
        outer header, and asks nothing of path discovery. A frame larger than
        a datagram is the sender's problem to split, and the link refuses to
        do it, because reassembly is a queue and a queue is a place to store
        an attacker's bytes.
*/
#define WATERLINK_DATAGRAM 1200

// What is left for frames once the cleartext header and the tag are taken.
#define WATERLINK_PAYLOAD (WATERLINK_DATAGRAM - 16 - 16)

/*
        The cleartext header, read before any key is in hand.

        receiver is the index the far side handed out at handshake: it says
        which session's key to try, so a machine holding many links does one
        table lookup rather than a trial decryption per peer. counter is the
        nonce, and it is also the replay window's sequence -- it never
        repeats within a session, and a session rekeys well before it could.
*/
struct waterlink_datagram {
        unsigned int kind;     // WATERLINK_KIND_*
        unsigned int receiver; // the far side's index for this session
        unsigned long counter; // nonce, and replay sequence
};

_Static_assert(sizeof(struct waterlink_datagram) == 16,
               "waterlink datagram header must be exactly 16 bytes");

#define WATERLINK_KIND_INITIATE 1u // Noise_IK message one
#define WATERLINK_KIND_RESPOND 2u  // Noise_IK message two
#define WATERLINK_KIND_CARRY 3u    // frames, once the session is up
#define WATERLINK_KIND_CLOSE 4u    // this session is finished

/*
        A frame, inside the box.

        length is the payload that follows this header, and a datagram holds
        as many whole frames as fit. deadline is milliseconds from the
        sender's own clock reading, not a timestamp: the two ends never have
        to agree on what time it is, only on how long a thing is worth
        waiting for. Zero means no deadline.
*/
struct waterlink_frame {
        unsigned long key;     // opaque to the link; the sender's meaning
        unsigned int sequence; // per key, increasing
        unsigned short channel;
        unsigned short length;
        unsigned short deadline; // milliseconds, 0 for none
        unsigned short flags;    // WATERLINK_FRAME_*
        unsigned short inflated; // bytes after unpacking, 0 when not packed
        unsigned short reserved; // must be 0
};

_Static_assert(sizeof(struct waterlink_frame) == 24,
               "waterlink frame header must be exactly 24 bytes");

/*
        A durable frame is never dropped and never superseded. A replaceable
        one may be dropped under the rule above, and is what state travels
        as. A frame with neither bit is refused rather than guessed at.
*/
#define WATERLINK_FRAME_REPLACEABLE 0x0001u
#define WATERLINK_FRAME_DURABLE 0x0002u

// The final frame for a key: the receiver may forget the key after it.
#define WATERLINK_FRAME_LAST 0x0004u

/*
        How the payload is packed, in the frame because the receiver must be
        able to undo it, and nowhere else. Three bits, so a method is a small
        number and not a negotiation.

        A receiver that does not know a method refuses the frame. It does not
        fall back and it does not ask: a sender only ever packs with a method
        the handshake said the far side has, and the handshake says that by
        version, not by a list -- a list is a negotiation and a negotiation is
        a downgrade.
*/
#define WATERLINK_FRAME_PACK_SHIFT 3
#define WATERLINK_FRAME_PACK_MASK 0x0038u

#define WATERLINK_PACK_NONE 0u
#define WATERLINK_PACK_DEFLATE 1u // gzip's coder, one frame at a time
#define WATERLINK_PACK_ZSTD 2u
#define WATERLINK_PACK_LZMA 3u // xz's coder, for durable bulk only

/*
        THE RULE THAT MAKES PACKING CORRECT, AND IS EASY TO GET WRONG

        A packed replaceable frame must be self contained.

        Every one of these coders earns its ratio from history: the window is
        what the last frames said. But a replaceable frame may be dropped --
        that is the whole point of the supersession rule above -- and a
        dropped frame takes the decoder's history with it, so every later
        frame on that key unpacks into garbage that the tag still says is
        authentic. A checksum will not save this; the bytes are genuinely
        what the sender sent.

        So history across frames is permitted only on a stream whose frames
        are all durable and therefore all ordered and all delivered. That is
        WATERLINK_STREAM_HISTORY below, and it is a promise about the frames,
        not a hint about the coder.

        Which also says plainly what packing is worth here: 1168 bytes with a
        fresh window is not much of a corpus. Packing pays on files and on the
        log, and it is close to free money on a terminal's own output, which
        is mostly spaces and repeated escapes. It does not pay on a desktop
        frame, which is already coded, and it does not pay on a keystroke.

        AND WHAT PACKING COSTS THAT IS NOT TIME

        A packed length is a measurement of the plaintext, and it is outside
        the box. The ceiling is not: a datagram is padded to
        WATERLINK_DATAGRAM, so a packed frame's length leaks nothing to an
        observer -- only to the far side, which already has the plaintext.
        The rule that keeps this true is that packing is per datagram and the
        datagram size is fixed, which is why the size is a constant and not a
        path discovery.

        The other cost is the receiver's. WATERLINK_PAYLOAD bounds what
        arrives, not what it becomes, and a paired peer is authenticated, not
        benign. So a frame declares what it unpacks to and the receiver
        refuses it before unpacking anything, against one ceiling and one
        scratch buffer taken at startup. Otherwise "the datapath allocates
        nothing" stops being true the first time somebody sends a well formed
        frame that claims a gigabyte.
*/
#define WATERLINK_INFLATED 65536


/*
        What a peer may do, granted one at a time and starting at none.

        VERBS is the moonwater vocabulary the machine already answers to, and
        it is what a freshly paired peer gets. It cannot execute, cannot open
        a terminal and cannot write a file. Everything past it is a decision
        somebody made on purpose.
*/
#define WATERLINK_MAY_VERBS 0x0001u
#define WATERLINK_MAY_RUN 0x0002u      // one command, no terminal
#define WATERLINK_MAY_SHELL 0x0004u    // a terminal
#define WATERLINK_MAY_SCREEN 0x0008u   // watch the desktop
#define WATERLINK_MAY_FILES 0x0010u    // push and pull
#define WATERLINK_MAY_LOG 0x0020u      // follow the kernel log
#define WATERLINK_MAY_CHANNELS 0x0040u // open channels of its own

#define WATERLINK_MAY_DEFAULT WATERLINK_MAY_VERBS

// The channels the link itself defines. An application opening its own
// channels under MAY_CHANNELS starts past these.
#define WATERLINK_CHANNEL_CONTROL 0u // verbs, and their answers
#define WATERLINK_CHANNEL_SHELL 1u
#define WATERLINK_CHANNEL_SCREEN 2u
#define WATERLINK_CHANNEL_LOG 3u
#define WATERLINK_CHANNEL_FILES 4u
#define WATERLINK_CHANNEL_OPEN 16u // the first an application may take

#define WATERLINK_KEY_BYTES 32 // X25519, and the AEAD key
#define WATERLINK_TAG_BYTES 16 // AES-GCM
#define WATERLINK_NAME_MAX 32  // what a peer is called here

/*
        A peer, as it is kept.

        Addresses are a cache and not an identity: a machine that moves keeps
        its key and loses its address, which is the ordinary case for anything
        on wifi and the reason a name is bound to a key rather than to a
        place. This record goes in the image's settings block beside the
        wireless networks and the timezone, so it survives an install the way
        those do.
*/
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

/*
        Rekeying, which is a correctness rule and not a policy.

        The counter is the nonce, so a session ends long before it could
        repeat one. These are WireGuard's numbers and they are its numbers
        for the same reason: whichever comes first, time or volume.
*/
#define WATERLINK_REKEY_MESSAGES (1UL << 40)
#define WATERLINK_REKEY_SECONDS 120
#define WATERLINK_REJECT_SECONDS 180

// How far behind the highest counter a datagram may arrive and still be new.
#define WATERLINK_REPLAY_WINDOW 2048

/*
        ------------------------------------------------------------------
        BELOW HERE IS WHAT THIS MACHINE KEEPS, NOT WHAT THE TWO ENDS AGREE ON
        ------------------------------------------------------------------

        Everything above is on the wire or is a rule both ends check. What
        follows is one sender's own settings, and the far side never sees any
        of it. That distinction is the point: a sender may change its mind
        about effort, order or packing between one frame and the next, and
        nothing has to be renegotiated, because there was never a negotiation.
        The only thing that crosses is the pack method in the frame, and that
        is there because unpacking needs it.

        STREAM SETTINGS, WHICH ARE CONTROLS AND NOT A STREAM TYPE

        A channel is not a kind of traffic. It is a channel with parameters,
        and the parameters can change under it: a terminal carrying a file for
        a moment is the ordinary case, not the exception. So there are no
        stream types here, only a small block of dials, and the things that
        look like types -- a shell, a desktop, a log -- are named presets that
        set those dials.

        A preset is a sender's convenience. It is not on the wire, it has no
        number the far side knows, and two machines running different presets
        interoperate exactly as well as two running the same one. Put a preset
        identifier on the wire and you have reinvented the version matrix that
        dropping cipher agility just bought us.
*/

/*
        Four dials, and each of them earns its place by naming a decision the
        other three cannot make.

        method  is what the receiver must know, and the only one on the wire.

        effort  is how hard to try, on the method's own scale, because no
                coder in this tree takes a time budget as input -- a
                microsecond figure would be nicer to reason about and would
                need a measured cost table per method per architecture that
                does not exist. So it is a level, and the honest part is the
                rule the scheduler applies around it: when a frame's deadline
                is closer than the last measured cost of packing at this
                effort, send it unpacked. Effort is what to do when there is
                time, not a promise about time.

        band    is the tiebreak, and only the tiebreak. The deadline already
                says when a frame is worth sending; the band says which frame
                goes first when two of them are both about to miss. Without
                it, a desktop that is always late starves a keystroke that is
                merely late.

        deadline is the stream's default for frames that name none, so that
                the common case -- a whole stream of frames that all want the
                same latency -- is a setting rather than a field repeated by
                every caller.

        There is deliberately no pacing share. Share of what a link has is a
        policy that deadline plus band already expresses for every case found
        so far, and a fourth control that overlaps the first two is how a
        scheduler becomes something nobody can predict.
*/
struct waterlink_stream {
        unsigned short channel;
        unsigned char method; // WATERLINK_PACK_*
        unsigned char effort; // 0 to 9, the method's own scale
        unsigned short deadline; // default for frames that give none
        unsigned char band;      // WATERLINK_BAND_*
        unsigned char flags;     // WATERLINK_STREAM_*
};

_Static_assert(sizeof(struct waterlink_stream) == 8,
               "waterlink stream settings must be exactly 8 bytes");

#define WATERLINK_BAND_URGENT 0u // a keystroke; nothing waits behind it
#define WATERLINK_BAND_LIVE 1u   // a frame that is worth less late
#define WATERLINK_BAND_BULK 2u   // a file; it wants throughput, not latency

/*
        Permission to keep a coder's window across frames. Legal only where
        every frame on the stream is durable, for the reason spelled out at
        WATERLINK_PACK_NONE above. A sender that sets this and then emits a
        replaceable frame has made the stream undecodable, so the sender
        refuses that frame rather than the receiver discovering it later.
*/
#define WATERLINK_STREAM_HISTORY 0x01u

// Pack even when the result is barely smaller. Off by default: below about a
// tenth saved, the far side's time to unpack costs more than the bytes did.
#define WATERLINK_STREAM_ALWAYS_PACK 0x02u

/*
        The presets. Sender side, and a starting point to edit rather than a
        closed set -- the whole reason the dials are separate from the names
        is so that content nobody anticipated gets its own block.

        SHELL       urgent, no deadline to speak of, packed cheaply because
                    terminal output is repetitive and a keystroke is too small
                    to pack at all.
        SCREEN      live, one frame time, unpacked: pixels arrive already
                    coded and packing them again spends time to grow them.
        LOG         bulk, packed hard with history, because a log is durable,
                    ordered, and the most compressible thing this machine
                    produces.
        FILES       bulk, packed hard with history, deadline none: a file is
                    worth waiting for and worth nothing half delivered.
        CONTROL     urgent, unpacked. A verb is shorter than a window.
        REALTIME    urgent, one tick, unpacked, and replaceable per frame --
                    a position that is 16 ms stale is not worth the bytes to
                    correct, which is the case the supersession rule was
                    written for.
*/
#define WATERLINK_PRESET_CONTROL 0u
#define WATERLINK_PRESET_SHELL 1u
#define WATERLINK_PRESET_SCREEN 2u
#define WATERLINK_PRESET_LOG 3u
#define WATERLINK_PRESET_FILES 4u
#define WATERLINK_PRESET_REALTIME 5u
#define WATERLINK_PRESET_COUNT 6u

static const struct waterlink_stream waterlink_presets[WATERLINK_PRESET_COUNT] = {
        [WATERLINK_PRESET_CONTROL] = { WATERLINK_CHANNEL_CONTROL,
                                       WATERLINK_PACK_NONE, 0, 0,
                                       WATERLINK_BAND_URGENT, 0 },
        [WATERLINK_PRESET_SHELL] = { WATERLINK_CHANNEL_SHELL,
                                     WATERLINK_PACK_DEFLATE, 1, 0,
                                     WATERLINK_BAND_URGENT, 0 },
        [WATERLINK_PRESET_SCREEN] = { WATERLINK_CHANNEL_SCREEN,
                                      WATERLINK_PACK_NONE, 0, 16,
                                      WATERLINK_BAND_LIVE, 0 },
        [WATERLINK_PRESET_LOG] = { WATERLINK_CHANNEL_LOG,
                                   WATERLINK_PACK_ZSTD, 6, 0,
                                   WATERLINK_BAND_BULK,
                                   WATERLINK_STREAM_HISTORY },
        [WATERLINK_PRESET_FILES] = { WATERLINK_CHANNEL_FILES,
                                     WATERLINK_PACK_ZSTD, 6, 0,
                                     WATERLINK_BAND_BULK,
                                     WATERLINK_STREAM_HISTORY },
        [WATERLINK_PRESET_REALTIME] = { WATERLINK_CHANNEL_OPEN,
                                        WATERLINK_PACK_NONE, 0, 16,
                                        WATERLINK_BAND_URGENT, 0 },
};

#endif // WATERLINK_INCLUDED
