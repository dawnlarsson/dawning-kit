/*
        Pairing two members of a group: Noise_XXpsk0_25519_AES128GCM_SHA256,
        the group's pairing key as the PSK.

        IK needs the far side's key in hand, and a machine meeting another
        member for the first time has nothing but the group. XX sends both
        static keys, and psk0 binds the whole exchange to the group from its
        first message: that message's payload is sealed under a key the PSK
        went into, so a machine without the secret is refused on one AEAD
        check before any Diffie-Hellman, and never sees a static key -- which
        would be a way to follow a machine around. (XXpsk3 would answer
        anyone's first message with the responder's static key and two
        curve multiplications.)

                -> psk, e                 message one: 16 bytes of tag only
                <- e, ee, s, es           two: the responder's key and name
                -> s, se                  three: the initiator's key and name

        A first message is refused a second time: the responder remembers the
        ephemerals it has answered, a fixed number of them. Not by timestamp,
        as the IK initiation is, because the responder does not know who is
        asking yet and a headless box may not have set its clock.

        After the third message each side keeps the other as an ordinary
        peer, with the grants the group was joined with, and every later
        session is the normal IK handshake over those keys. The transport
        keys this exchange makes are not used for anything.

        Same cipher variant as handshake.c, same reasons; the name is longer
        than a hash, so its hash is the first h.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_PAIR_INCLUDED
#define WATERLINK_PAIR_INCLUDED

#include "handshake.c"

#define WATERLINK_PAIR_PROTOCOL "Noise_XXpsk0_25519_AES128GCM_SHA256"
#define WATERLINK_PAIR_PROLOGUE "waterlink pair 1"

#define WATERLINK_KIND_PAIR_1 5u
#define WATERLINK_KIND_PAIR_2 6u
#define WATERLINK_KIND_PAIR_3 7u

// A machine's name as it offers it; the receiver still checks it.
#define WATERLINK_PAIR_NAME 32

#define WATERLINK_PAIR_1_BYTES (32 + 16)
#define WATERLINK_PAIR_2_BYTES (32 + 48 + WATERLINK_PAIR_NAME + 16)
#define WATERLINK_PAIR_3_BYTES (48 + WATERLINK_PAIR_NAME + 16)

// MixKeyAndHash: three outputs, the middle one into the hash.
static fn waterlink_mix_key_hash(struct waterlink_noise address_to noise,
                                 p8 address_to material, positive length)
{
        p8 prk[32];
        p8 out[96];

        crypto_hkdf_extract(noise->chain, 32, material, length, prk);
        crypto_hkdf_expand(prk, (p8 address_to) "", 0, out, 96);
        memory_copy(noise->chain, out, 32);
        waterlink_mix_hash(noise, out + 32, 32);
        memory_copy(noise->key, out + 64, 32);
        noise->keyed = true;
        noise->nonce = 0;
        crypto_forget(prk, sizeof prk);
        crypto_forget(out, sizeof out);
}

static fn waterlink_pair_start(struct waterlink_noise address_to noise,
                               p8 address_to psk)
{
        memory_zero(noise, sizeof(address_to noise));
        crypto_sha256_of((p8 address_to)WATERLINK_PAIR_PROTOCOL,
                         sizeof(WATERLINK_PAIR_PROTOCOL) - 1, noise->hash);
        memory_copy(noise->chain, noise->hash, 32);
        waterlink_mix_hash(noise, (p8 address_to)WATERLINK_PAIR_PROLOGUE,
                           sizeof(WATERLINK_PAIR_PROLOGUE) - 1);
        waterlink_mix_key_hash(noise, psk, 32);
}

//      A psk pattern's "e": into the hash, and into the key.
static fn waterlink_pair_e(struct waterlink_noise address_to noise,
                           p8 address_to public)
{
        waterlink_mix_hash(noise, public, 32);
        waterlink_mix_key(noise, public, 32);
}

static fn waterlink_pair_head(p8 address_to datagram, p32 kind, p32 receiver)
{
        struct waterlink_datagram head = {kind, receiver, 0};

        memory_zero(datagram, WATERLINK_DATAGRAM);
        memory_copy(datagram, address_of head, 16);
}

// Message one, from the machine that saw the other's announcement.
fn waterlink_pair_first(struct waterlink_noise address_to noise,
                        p8 address_to psk, p8 address_to ephemeral,
                        p32 our_index, p8 address_to datagram)
{
        p8 address_to at = datagram + 16;

        waterlink_pair_head(datagram, WATERLINK_KIND_PAIR_1, our_index);
        waterlink_pair_start(noise, psk);

        memory_copy(noise->ephemeral, ephemeral, 32);
        crypto_x25519(noise->ephemeral_public, noise->ephemeral,
                      waterlink_base);
        memory_copy(at, noise->ephemeral_public, 32);
        waterlink_pair_e(noise, at);
        waterlink_seal_hash(noise, at + 32, 0);
}

/*
        Message one, at the responder, for one group's key: true when it was
        sealed under that group's PSK. The cheap question, and the only one
        asked of a stranger.
*/
bool waterlink_pair_heard_first(struct waterlink_noise address_to noise,
                                p8 address_to psk, p8 address_to datagram)
{
        p8 address_to at = datagram + 16;
        p8 nothing[1];

        waterlink_pair_start(noise, psk);
        memory_copy(noise->remote_ephemeral, at, 32);
        waterlink_pair_e(noise, at);
        return waterlink_open_hash(noise, at + 32, 0, nothing);
}

bool waterlink_pair_second(struct waterlink_noise address_to noise,
                           struct waterlink_identity address_to me,
                           p8 address_to ephemeral, p8 address_to name,
                           p32 their_index, p8 address_to datagram)
{
        p8 address_to at = datagram + 16;

        waterlink_pair_head(datagram, WATERLINK_KIND_PAIR_2, their_index);

        // <- e
        memory_copy(noise->ephemeral, ephemeral, 32);
        crypto_x25519(noise->ephemeral_public, noise->ephemeral,
                      waterlink_base);
        memory_copy(at, noise->ephemeral_public, 32);
        waterlink_pair_e(noise, at);
        at += 32;

        // ee
        if (!waterlink_mix_dh(noise, noise->ephemeral,
                              noise->remote_ephemeral))
                return false;

        // s
        memory_copy(at, me->public, 32);
        waterlink_seal_hash(noise, at, 32);
        at += 48;

        // es: the initiator's ephemeral with our static
        if (!waterlink_mix_dh(noise, me->secret, noise->remote_ephemeral))
                return false;

        memory_copy(at, name, WATERLINK_PAIR_NAME);
        waterlink_seal_hash(noise, at, WATERLINK_PAIR_NAME);
        return true;
}

bool waterlink_pair_heard_second(struct waterlink_noise address_to noise,
                                 p8 address_to datagram,
                                 p8 address_to their_public,
                                 p8 address_to their_name)
{
        p8 address_to at = datagram + 16;

        memory_copy(noise->remote_ephemeral, at, 32);
        waterlink_pair_e(noise, at);
        at += 32;

        if (!waterlink_mix_dh(noise, noise->ephemeral,
                              noise->remote_ephemeral))
                return false;

        if (!waterlink_open_hash(noise, at, 32, noise->remote_static))
                return false;
        at += 48;

        if (!waterlink_mix_dh(noise, noise->ephemeral, noise->remote_static))
                return false;

        if (!waterlink_open_hash(noise, at, WATERLINK_PAIR_NAME, their_name))
                return false;

        memory_copy(their_public, noise->remote_static, 32);
        return true;
}

bool waterlink_pair_third(struct waterlink_noise address_to noise,
                          struct waterlink_identity address_to me,
                          p8 address_to name, p32 their_index,
                          p8 address_to datagram)
{
        p8 address_to at = datagram + 16;

        waterlink_pair_head(datagram, WATERLINK_KIND_PAIR_3, their_index);

        // s
        memory_copy(at, me->public, 32);
        waterlink_seal_hash(noise, at, 32);
        at += 48;

        // se: our static with the responder's ephemeral
        if (!waterlink_mix_dh(noise, me->secret, noise->remote_ephemeral))
                return false;

        memory_copy(at, name, WATERLINK_PAIR_NAME);
        waterlink_seal_hash(noise, at, WATERLINK_PAIR_NAME);
        return true;
}

bool waterlink_pair_heard_third(struct waterlink_noise address_to noise,
                                struct waterlink_identity address_to me,
                                p8 address_to datagram,
                                p8 address_to their_public,
                                p8 address_to their_name)
{
        p8 address_to at = datagram + 16;

        if (!waterlink_open_hash(noise, at, 32, noise->remote_static))
                return false;
        at += 48;

        if (!waterlink_mix_dh(noise, noise->ephemeral, noise->remote_static))
                return false;

        if (!waterlink_open_hash(noise, at, WATERLINK_PAIR_NAME, their_name))
                return false;

        memory_copy(their_public, noise->remote_static, 32);
        (void)me;
        return true;
}

/*
        The first messages a responder has answered, by ephemeral: a message
        seen again is a replay, whoever sends it.
*/
#define WATERLINK_PAIR_SEEN 64

struct waterlink_pair_seen {
        p8 ephemeral[WATERLINK_PAIR_SEEN][32];
        positive next;
        positive count;
};

bool waterlink_pair_fresh(struct waterlink_pair_seen address_to seen,
                          p8 address_to ephemeral)
{
        for (positive at = 0; at < seen->count; at++)
                if (!memory_compare(seen->ephemeral[at], ephemeral, 32))
                        return false;

        memory_copy(seen->ephemeral[seen->next], ephemeral, 32);
        seen->next = (seen->next + 1) % WATERLINK_PAIR_SEEN;
        if (seen->count < WATERLINK_PAIR_SEEN)
                seen->count++;
        return true;
}

#endif // WATERLINK_PAIR_INCLUDED
