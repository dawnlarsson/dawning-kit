/*
        Sealing and opening a waterlink datagram, over lib.c's assembly.

        AES-128-GCM, shaped to the datagram rather than to a general record.
        Because the datagram is whole blocks and the cleartext header is
        exactly one of them (waterlink.c says why), the header, box and tag
        sit at fixed offsets and the whole operation is three calls into
        lib.c: aes128_ctr_blocks once, ghash_blocks once. A datagram of
        acknowledgements alone is the same shape cut shorter. The general path in
        net.c pays for an arbitrary record -- a separate call for the tag's
        block, padding for partial blocks, and a framing layer around both --
        and a datagram needs none of it.

        The one trick is the tag's mask. GCM encrypts counter one for the tag
        and counters two onward for the text, which is one counter-mode run
        starting at one if the block in front of the text is zero. The header
        is that block: it is set aside, the slot is zeroed, seventy four
        blocks go through in a single call, and the first block out is the
        mask. Then the header comes back and is hashed as what it always was,
        the associated data.

        This is included where lib.c and net.c's crypto are already in scope;
        the prepared key is net.c's crypto_aesgcm_key.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_SEAL_INCLUDED
#define WATERLINK_SEAL_INCLUDED

#include "waterlink.c"

#define WATERLINK_BLOCKS (WATERLINK_DATAGRAM / 16)
#define WATERLINK_BOXED (1 + WATERLINK_PAYLOAD / 16) // header and box

/*
        The nonce is the counter, little endian, behind four zero bytes --
        WireGuard's layout. Each direction of a session has its own key, so
        two ends counting up from one never use a nonce twice under one key.
        J0 is that, with GCM's block counter at one.
*/
static fn waterlink_counter_block(p8 address_to block, p64 counter)
{
        memory_zero(block, 4);
        for (positive i = 0; i < 8; i++)
                block[4 + i] = (p8)(counter >> (8 * i));
        block[12] = 0;
        block[13] = 0;
        block[14] = 0;
        block[15] = 1;
}

/*
        GHASH over the header, the box and GCM's lengths block, in one call.
        The lengths block has to follow the box, and the tag's slot already
        does: so it is written there, the whole datagram is hashed as one
        run of blocks, and the slot is given back. One call instead of two is
        one reduction of the state instead of two, which is the part of
        hashing a datagram that nothing runs beside.

        The header is the associated data, so it is 128 bits of it; the box is
        the text. The tag slot's contents are handed back through kept, since
        opening needs the tag that arrived after the slot has been borrowed.
*/
static fn waterlink_authenticate(crypto_aesgcm_key address_to key,
                                 p8 address_to datagram, positive box,
                                 p8 address_to state, p8 address_to kept)
{
        p8 address_to slot = datagram + 16 + box;

        memory_copy(kept, slot, 16);
        memory_zero(slot, 16);
        slot[7] = 128;
        slot[13] = (p8)((box * 8) >> 16);
        slot[14] = (p8)((box * 8) >> 8);
        slot[15] = (p8)(box * 8);

        memory_zero(state, 16);
        ghash_blocks(state, key->table, datagram, 2 + box / 16);

        memory_copy(slot, kept, 16);
}

/*
        Run the counter over header-and-box in place with the header's slot
        zeroed, which leaves the tag's mask where the header was and the box
        encrypted or decrypted after it -- counter mode does not know which.
        The header goes back before returning; the mask is handed out.
*/
static fn waterlink_counter_run(crypto_aesgcm_key address_to key,
                                p8 address_to datagram, positive box,
                                p64 counter, p8 address_to mask)
{
        p8 block[16];
        p8 header[16];

        memory_copy(header, datagram, 16);
        memory_zero(datagram, 16);

        waterlink_counter_block(block, counter);
        aes128_ctr_blocks(key->round, block, datagram, datagram,
                          1 + box / 16);

        memory_copy(mask, datagram, 16);
        memory_copy(datagram, header, 16);
        crypto_forget(block, sizeof block);
}

static fn waterlink_seal_box(crypto_aesgcm_key address_to key,
                             p8 address_to datagram, positive used,
                             positive box)
{
        struct waterlink_datagram head;
        p8 mask[16];
        p8 state[16];
        p8 kept[16];

        memory_copy(address_of head, datagram, 16);

        if (used < box)
                memory_zero(datagram + 16 + used, box - used);

        waterlink_counter_run(key, datagram, box, head.counter, mask);
        waterlink_authenticate(key, datagram, box, state, kept);

        for (positive i = 0; i < 16; i++)
                datagram[16 + box + i] = state[i] ^ mask[i];

        crypto_forget(mask, sizeof mask);
        crypto_forget(state, sizeof state);
}

/*
        Seal a datagram whose header is written and whose box holds used
        bytes of frames. The rest of the box is zeroed first -- padding is
        part of what is authenticated, and a box that ends in leftover bytes
        from the last datagram would be sealing whatever they were.
*/
fn waterlink_seal(crypto_aesgcm_key address_to key, p8 address_to datagram,
                  positive used)
{
        waterlink_seal_box(key, datagram, used, WATERLINK_PAYLOAD);
}

/*
        A datagram that carries only acknowledgements is cut to whole blocks
        instead of padded. It says nothing its timing does not already say --
        that the other way is carrying something -- and at full size it made
        the return path carry as many bytes as the forward one, so on any
        path slower one way than the other the acknowledgements became the
        bottleneck and their losses looked like lost data. Returns the
        datagram's length, a multiple of sixteen from 48 to the full size.
*/
positive waterlink_seal_short(crypto_aesgcm_key address_to key,
                              p8 address_to datagram, positive used)
{
        positive box = (used + 15) & ~(positive)15;

        if (!box)
                box = 16;
        if (box > WATERLINK_PAYLOAD)
                box = WATERLINK_PAYLOAD;

        waterlink_seal_box(key, datagram, used, box);
        return 16 + box + 16;
}

/*
        Open a datagram of length bytes in place. The tag is checked over the
        ciphertext before anything is trusted, compared whole so a wrong tag
        says nothing about where it went wrong, and on failure the box is
        wiped: the counter run has already turned it into plaintext-shaped
        bytes, and unauthenticated plaintext is not something to leave lying
        in a buffer the caller might read.

        Returns false for a tag that does not verify, or a length that is not
        a datagram's. The counter is only trustworthy after this returns true,
        which is why the replay window is asked afterwards and never before.
*/
bool waterlink_open_length(crypto_aesgcm_key address_to key,
                           p8 address_to datagram, positive length)
{
        struct waterlink_datagram head;
        positive box;
        p8 mask[16];
        p8 state[16];
        p8 tag[16];
        bool good;

        if (length < 48 || length > WATERLINK_DATAGRAM || length % 16)
                return false;

        box = length - 32;
        memory_copy(address_of head, datagram, 16);

        waterlink_authenticate(key, datagram, box, state, tag);
        waterlink_counter_run(key, datagram, box, head.counter, mask);

        for (positive i = 0; i < 16; i++)
                state[i] ^= mask[i];

        good = crypto_same(state, tag, 16);
        if (!good)
                memory_zero(datagram + 16, box);

        crypto_forget(mask, sizeof mask);
        crypto_forget(state, sizeof state);
        return good;
}

bool waterlink_open(crypto_aesgcm_key address_to key, p8 address_to datagram)
{
        return waterlink_open_length(key, datagram, WATERLINK_DATAGRAM);
}

#endif // WATERLINK_SEAL_INCLUDED
