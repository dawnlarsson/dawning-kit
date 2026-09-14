/*
        Hashes, AES-GCM, X25519 and the signature checks HTTPS needs.

        wget speaks TLS 1.3 with AES-128-GCM, X25519, P-256 and P-384. The chain for that
        handshake is ECDSA on P-256 and P-384, RSA PKCS#1 and RSA-PSS SHA-256;
        Alpine and GitHub still present RSA leaves. SHA-256 compression is
        sha256_compress in library.c, on the same hardware floor as the rest
        of the binary, and SHA-384 is sha512_blocks through the same streaming
        digests the checksum utilities use. X25519 and ECDSA stay C for now. None of
        this is a kernel crypto ABI: AF_ALG is off on Moonwater, and a
        downloader cannot wait on it.
*/

#ifndef STANDARD_MODERN_C_NET_CRYPTO
#define STANDARD_MODERN_C_NET_CRYPTO

typedef unsigned __int128 crypto_wide;

static p32 crypto_be32(const p8 address_to bytes)
{
        return ((p32)bytes[0] << 24) | ((p32)bytes[1] << 16) |
               ((p32)bytes[2] << 8) | (p32)bytes[3];
}

static p64 crypto_be64(const p8 address_to bytes)
{
        return ((p64)crypto_be32(bytes) << 32) | crypto_be32(bytes + 4);
}

static fn crypto_put_be32(p8 address_to bytes, p32 value)
{
        bytes[0] = (p8)(value >> 24);
        bytes[1] = (p8)(value >> 16);
        bytes[2] = (p8)(value >> 8);
        bytes[3] = (p8)value;
}

static fn crypto_put_be64(p8 address_to bytes, p64 value)
{
        crypto_put_be32(bytes, (p32)(value >> 32));
        crypto_put_be32(bytes + 4, (p32)value);
}

/*
        SHA-256 for the transcript, HKDF and the signature hashes, SHA-384 for
        the P-384 and RSA-SHA384 chains: the library's streaming digests over
        sha256_blocks and sha512_blocks, under the names this file has always
        used. A transcript copy is a digest_state copy.
*/
typedef digest_state crypto_sha256;
typedef digest_state crypto_sha512;

static fn crypto_sha256_open(crypto_sha256 address_to hash)
{
        digest_open(hash, DIGEST_SHA256, 32);
}

static fn crypto_sha256_write(crypto_sha256 address_to hash, p8 address_to data,
                              positive length)
{
        digest_write(hash, data, length);
}

static fn crypto_sha256_close(crypto_sha256 address_to hash, p8 address_to out)
{
        digest_close(hash, out);
}

static fn crypto_sha256_of(p8 address_to data, positive length, p8 address_to out)
{
        crypto_sha256 hash;

        crypto_sha256_open(address_of hash);
        crypto_sha256_write(address_of hash, data, length);
        crypto_sha256_close(address_of hash, out);
}

static fn crypto_sha384_open(crypto_sha512 address_to hash)
{
        digest_open(hash, DIGEST_SHA384, 48);
}

static fn crypto_sha512_write(crypto_sha512 address_to hash, p8 address_to data,
                              positive length)
{
        digest_write(hash, data, length);
}

static fn crypto_sha384_close(crypto_sha512 address_to hash, p8 address_to out)
{
        digest_close(hash, out);
}

static fn crypto_sha384(p8 address_to data, positive length, p8 address_to out)
{
        crypto_sha512 hash;

        crypto_sha384_open(address_of hash);
        crypto_sha512_write(address_of hash, data, length);
        crypto_sha384_close(address_of hash, out);
}

static fn crypto_forget(address_any secret, positive length);

static fn crypto_hmac_sha256(p8 address_to key, positive key_length,
                             p8 address_to data, positive length,
                             p8 address_to out)
{
        crypto_sha256 inner;
        crypto_sha256 outer;
        p8 pad[64];
        p8 inner_sum[32];
        p8 key_block[64];
        p8 hashed[32];
        positive i;

        memory_fill(key_block, 0, 64);
        if (key_length > 64)
        {
                crypto_sha256_of(key, key_length, hashed);
                memory_copy(key_block, hashed, 32);
        }
        else
                memory_copy(key_block, key, key_length);

        for (i = 0; i < 64; i++)
                pad[i] = key_block[i] ^ 0x36;

        crypto_sha256_open(address_of inner);
        crypto_sha256_write(address_of inner, pad, 64);
        crypto_sha256_write(address_of inner, data, length);
        crypto_sha256_close(address_of inner, inner_sum);

        for (i = 0; i < 64; i++)
                pad[i] = key_block[i] ^ 0x5c;

        crypto_sha256_open(address_of outer);
        crypto_sha256_write(address_of outer, pad, 64);
        crypto_sha256_write(address_of outer, inner_sum, 32);
        crypto_sha256_close(address_of outer, out);

        crypto_forget(address_of inner, sizeof inner);
        crypto_forget(address_of outer, sizeof outer);
        crypto_forget(pad, sizeof pad);
        crypto_forget(inner_sum, sizeof inner_sum);
        crypto_forget(key_block, sizeof key_block);
        crypto_forget(hashed, sizeof hashed);
}

static fn crypto_hkdf_extract(p8 address_to salt, positive salt_length,
                              p8 address_to ikm, positive ikm_length,
                              p8 address_to prk)
{
        static p8 zeros[32];

        if (!salt || !salt_length)
        {
                salt = zeros;
                salt_length = 32;
        }

        crypto_hmac_sha256(salt, salt_length, ikm, ikm_length, prk);
}

static fn crypto_hkdf_expand(p8 address_to prk, p8 address_to info,
                             positive info_length, p8 address_to out,
                             positive out_length)
{
        p8 previous[32];
        p8 block[32];
        positive have = 0;
        p8 counter = 1;

        while (have < out_length)
        {
                crypto_sha256 hash;
                p8 pad[64];
                p8 inner[32];
                positive i;
                p8 key_block[64];

                memory_copy(key_block, prk, 32);
                memory_fill(key_block + 32, 0, 32);

                for (i = 0; i < 64; i++)
                        pad[i] = key_block[i] ^ 0x36;

                crypto_sha256_open(address_of hash);
                crypto_sha256_write(address_of hash, pad, 64);
                if (counter > 1)
                        crypto_sha256_write(address_of hash, previous, 32);
                crypto_sha256_write(address_of hash, info, info_length);
                crypto_sha256_write(address_of hash, address_of counter, 1);
                crypto_sha256_close(address_of hash, inner);

                for (i = 0; i < 64; i++)
                        pad[i] = key_block[i] ^ 0x5c;

                crypto_sha256_open(address_of hash);
                crypto_sha256_write(address_of hash, pad, 64);
                crypto_sha256_write(address_of hash, inner, 32);
                crypto_sha256_close(address_of hash, block);

                {
                        positive take = out_length - have;

                        if (take > 32)
                                take = 32;
                        memory_copy(out + have, block, take);
                        have += take;
                }

                memory_copy(previous, block, 32);
                counter++;

                crypto_forget(address_of hash, sizeof hash);
                crypto_forget(pad, sizeof pad);
                crypto_forget(inner, sizeof inner);
                crypto_forget(key_block, sizeof key_block);
        }

        crypto_forget(previous, sizeof previous);
        crypto_forget(block, sizeof block);
}

/* The AES state and key are secret, so an ordinary S-box table exposes them
   through the cache.  Invert in GF(2^8) with a fixed addition chain, then
   apply the AES affine transform.  Every input follows the same operations
   and addresses. */
static p8 crypto_aes_field_multiply(p8 left, p8 right)
{
        p8 product = 0;
        positive bit;

        for (bit = 0; bit < 8; bit++)
        {
                p8 selected = (p8)(0 - (right & 1));
                p8 high = left >> 7;

                product ^= left & selected;
                left = (p8)((left << 1) ^
                            (0x1b & (p8)(0 - high)));
                right >>= 1;
        }

        return product;
}

static p8 crypto_aes_substitute(p8 value)
{
        p8 x2 = crypto_aes_field_multiply(value, value);
        p8 x3 = crypto_aes_field_multiply(x2, value);
        p8 x6 = crypto_aes_field_multiply(x3, x3);
        p8 x12 = crypto_aes_field_multiply(x6, x6);
        p8 x15 = crypto_aes_field_multiply(x12, x3);
        p8 x30 = crypto_aes_field_multiply(x15, x15);
        p8 x60 = crypto_aes_field_multiply(x30, x30);
        p8 x120 = crypto_aes_field_multiply(x60, x60);
        p8 x240 = crypto_aes_field_multiply(x120, x120);
        p8 inverse = crypto_aes_field_multiply(
            crypto_aes_field_multiply(x240, x12), x2);

        return (p8)(inverse ^
                    ((inverse << 1) | (inverse >> 7)) ^
                    ((inverse << 2) | (inverse >> 6)) ^
                    ((inverse << 3) | (inverse >> 5)) ^
                    ((inverse << 4) | (inverse >> 4)) ^ 0x63);
}

/* Unlike memory_fill, these stores cannot be discarded after the final use. */
static fn crypto_forget(address_any secret, positive length)
{
        volatile p8 address_to at = secret;

        while (length)
        {
                *at++ = 0;
                length--;
        }
}

static fn crypto_aes128_expand(p8 address_to key, p8 address_to round)
{
        static const p8 rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                    0x20, 0x40, 0x80, 0x1b, 0x36};
        positive i;

        memory_copy(round, key, 16);
        for (i = 16; i < 176; i += 4)
        {
                p8 t0 = round[i - 4];
                p8 t1 = round[i - 3];
                p8 t2 = round[i - 2];
                p8 t3 = round[i - 1];

                if (i % 16 == 0)
                {
                        p8 k = t0;
                        t0 = crypto_aes_substitute(t1) ^ rcon[i / 16 - 1];
                        t1 = crypto_aes_substitute(t2);
                        t2 = crypto_aes_substitute(t3);
                        t3 = crypto_aes_substitute(k);
                }

                round[i] = round[i - 16] ^ t0;
                round[i + 1] = round[i - 15] ^ t1;
                round[i + 2] = round[i - 14] ^ t2;
                round[i + 3] = round[i - 13] ^ t3;
        }
}

/*
        GHASH over a span, the last partial block zero padded. The multiply
        is ghash_blocks in library.c over the table ghash_key made; GCM's
        framing stays here.
*/
static fn crypto_ghash_span(p8 address_to state, p8 address_to table,
                            p8 address_to bytes, positive length)
{
        positive whole = length / 16;
        p8 padded[16];

        ghash_blocks(state, table, bytes, whole);

        if (length % 16)
        {
                memory_fill(padded, 0, 16);
                memory_copy(padded, bytes + whole * 16, length % 16);
                ghash_blocks(state, table, padded, 1);
                crypto_forget(padded, sizeof padded);
        }
}

/*
        An AES-128-GCM key prepared once: the FIPS-197 schedule and the GHASH
        table of H's powers. Preparing costs the key expansion, one block and
        forty seven carry-less multiplies, which a connection pays when it
        installs a traffic key rather than on every record. It is key
        material; wipe it with the key.
*/
typedef struct
{
        p8 round[176];
        p8 table[GHASH_KEY_SIZE] __attribute__((aligned(64)));
} crypto_aesgcm_key;

static fn crypto_aesgcm_prepare(crypto_aesgcm_key address_to key,
                                p8 address_to raw)
{
        p8 counter[16];
        p8 h[16];

        crypto_aes128_expand(raw, key->round);
        memory_fill(counter, 0, 16);
        memory_fill(h, 0, 16);
        aes128_ctr_blocks(key->round, counter, h, h, 1);
        ghash_key(key->table, h);
        crypto_forget(h, sizeof h);
}

static fn crypto_aesgcm_crypt(crypto_aesgcm_key address_to key,
                              p8 address_to iv, p8 address_to aad,
                              positive aad_length, p8 address_to text,
                              positive text_length, p8 address_to tag,
                              bool encrypt)
{
        p8 j0[16];
        p8 counter[16];
        p8 s[16];
        p8 padded[16];
        positive whole = text_length / 16;
        positive rest = text_length % 16;

        memory_copy(j0, iv, 12);
        j0[12] = 0;
        j0[13] = 0;
        j0[14] = 0;
        j0[15] = 1;
        memory_copy(counter, j0, 16);
        counter[15] = 2;
        memory_fill(s, 0, 16);

        crypto_ghash_span(s, key->table, aad, aad_length);

        //      GHASH reads the ciphertext both ways: before the counter
        //      stream comes off it on the way in, after it goes on on the
        //      way out.
        if (!encrypt)
                crypto_ghash_span(s, key->table, text, text_length);

        aes128_ctr_blocks(key->round, counter, text, text, whole);
        if (rest)
        {
                memory_fill(padded, 0, 16);
                memory_copy(padded, text + whole * 16, rest);
                aes128_ctr_blocks(key->round, counter, padded, padded, 1);
                memory_copy(text + whole * 16, padded, rest);
        }

        if (encrypt)
                crypto_ghash_span(s, key->table, text, text_length);

        memory_fill(padded, 0, 16);
        crypto_put_be64(padded, (p64)aad_length * 8);
        crypto_put_be64(padded + 8, (p64)text_length * 8);
        ghash_blocks(s, key->table, padded, 1);

        aes128_ctr_blocks(key->round, j0, s, tag, 1);

        crypto_forget(j0, sizeof j0);
        crypto_forget(counter, sizeof counter);
        crypto_forget(s, sizeof s);
        crypto_forget(padded, sizeof padded);
}

static fn crypto_aesgcm_seal(crypto_aesgcm_key address_to key,
                             p8 address_to iv, p8 address_to aad,
                             positive aad_length, p8 address_to text,
                             positive text_length, p8 address_to tag)
{
        crypto_aesgcm_crypt(key, iv, aad, aad_length, text, text_length, tag,
                            true);
}

static bool crypto_aesgcm_open(crypto_aesgcm_key address_to key,
                               p8 address_to iv, p8 address_to aad,
                               positive aad_length, p8 address_to text,
                               positive text_length, p8 address_to tag)
{
        p8 got[16];
        positive i;
        p8 diff = 0;
        bool valid;

        crypto_aesgcm_crypt(key, iv, aad, aad_length, text, text_length, got,
                            false);
        for (i = 0; i < 16; i++)
                diff |= got[i] ^ tag[i];
        valid = diff == 0;
        if (!valid)
                crypto_forget(text, text_length);
        crypto_forget(got, sizeof got);
        crypto_forget(address_of diff, sizeof diff);
        return valid;
}

/*
        RFC 7748 X25519 on 5 x 51-bit limbs. Field reduce, freeze and the
        Montgomery step follow the public-domain 64-bit curve25519-donna.
*/
typedef p64 crypto_x25519_fe[5];

static p64 crypto_x25519_load64(p8 address_to in)
{
        return (p64)in[0] | ((p64)in[1] << 8) | ((p64)in[2] << 16) |
               ((p64)in[3] << 24) | ((p64)in[4] << 32) | ((p64)in[5] << 40) |
               ((p64)in[6] << 48) | ((p64)in[7] << 56);
}

static fn crypto_x25519_store64(p8 address_to out, p64 in)
{
        out[0] = (p8)in;
        out[1] = (p8)(in >> 8);
        out[2] = (p8)(in >> 16);
        out[3] = (p8)(in >> 24);
        out[4] = (p8)(in >> 32);
        out[5] = (p8)(in >> 40);
        out[6] = (p8)(in >> 48);
        out[7] = (p8)(in >> 56);
}

static fn crypto_x25519_copy(crypto_x25519_fe o, crypto_x25519_fe a)
{
        o[0] = a[0];
        o[1] = a[1];
        o[2] = a[2];
        o[3] = a[3];
        o[4] = a[4];
}

static fn crypto_x25519_load(crypto_x25519_fe out, p8 address_to in)
{
        out[0] = crypto_x25519_load64(in) & 0x7ffffffffffffull;
        out[1] = (crypto_x25519_load64(in + 6) >> 3) & 0x7ffffffffffffull;
        out[2] = (crypto_x25519_load64(in + 12) >> 6) & 0x7ffffffffffffull;
        out[3] = (crypto_x25519_load64(in + 19) >> 1) & 0x7ffffffffffffull;
        out[4] = (crypto_x25519_load64(in + 24) >> 12) & 0x7ffffffffffffull;
}

static fn crypto_x25519_store(p8 address_to out, crypto_x25519_fe in)
{
        crypto_wide t[5];

        t[0] = in[0];
        t[1] = in[1];
        t[2] = in[2];
        t[3] = in[3];
        t[4] = in[4];

        t[1] += t[0] >> 51;
        t[0] &= 0x7ffffffffffffull;
        t[2] += t[1] >> 51;
        t[1] &= 0x7ffffffffffffull;
        t[3] += t[2] >> 51;
        t[2] &= 0x7ffffffffffffull;
        t[4] += t[3] >> 51;
        t[3] &= 0x7ffffffffffffull;
        t[0] += 19 * (t[4] >> 51);
        t[4] &= 0x7ffffffffffffull;

        t[1] += t[0] >> 51;
        t[0] &= 0x7ffffffffffffull;
        t[2] += t[1] >> 51;
        t[1] &= 0x7ffffffffffffull;
        t[3] += t[2] >> 51;
        t[2] &= 0x7ffffffffffffull;
        t[4] += t[3] >> 51;
        t[3] &= 0x7ffffffffffffull;
        t[0] += 19 * (t[4] >> 51);
        t[4] &= 0x7ffffffffffffull;

        t[0] += 19;

        t[1] += t[0] >> 51;
        t[0] &= 0x7ffffffffffffull;
        t[2] += t[1] >> 51;
        t[1] &= 0x7ffffffffffffull;
        t[3] += t[2] >> 51;
        t[2] &= 0x7ffffffffffffull;
        t[4] += t[3] >> 51;
        t[3] &= 0x7ffffffffffffull;
        t[0] += 19 * (t[4] >> 51);
        t[4] &= 0x7ffffffffffffull;

        t[0] += 0x8000000000000ull - 19;
        t[1] += 0x8000000000000ull - 1;
        t[2] += 0x8000000000000ull - 1;
        t[3] += 0x8000000000000ull - 1;
        t[4] += 0x8000000000000ull - 1;

        t[1] += t[0] >> 51;
        t[0] &= 0x7ffffffffffffull;
        t[2] += t[1] >> 51;
        t[1] &= 0x7ffffffffffffull;
        t[3] += t[2] >> 51;
        t[2] &= 0x7ffffffffffffull;
        t[4] += t[3] >> 51;
        t[3] &= 0x7ffffffffffffull;
        t[4] &= 0x7ffffffffffffull;

        crypto_x25519_store64(out, (p64)(t[0] | (t[1] << 51)));
        crypto_x25519_store64(out + 8, (p64)((t[1] >> 13) | (t[2] << 38)));
        crypto_x25519_store64(out + 16, (p64)((t[2] >> 26) | (t[3] << 25)));
        crypto_x25519_store64(out + 24, (p64)((t[3] >> 39) | (t[4] << 12)));
        crypto_forget(t, sizeof t);
}

static fn crypto_x25519_sum(crypto_x25519_fe o, crypto_x25519_fe in)
{
        o[0] += in[0];
        o[1] += in[1];
        o[2] += in[2];
        o[3] += in[3];
        o[4] += in[4];
}

static fn crypto_x25519_diff(crypto_x25519_fe o, crypto_x25519_fe in)
{
        o[0] = in[0] + 0x3fffffffffff68ull - o[0];
        o[1] = in[1] + 0x3ffffffffffff8ull - o[1];
        o[2] = in[2] + 0x3ffffffffffff8ull - o[2];
        o[3] = in[3] + 0x3ffffffffffff8ull - o[3];
        o[4] = in[4] + 0x3ffffffffffff8ull - o[4];
}

static fn crypto_x25519_mul(crypto_x25519_fe o, crypto_x25519_fe in2,
                            crypto_x25519_fe in)
{
        crypto_wide t[5];
        p64 r0, r1, r2, r3, r4, s0, s1, s2, s3, s4, c;

        r0 = in[0];
        r1 = in[1];
        r2 = in[2];
        r3 = in[3];
        r4 = in[4];
        s0 = in2[0];
        s1 = in2[1];
        s2 = in2[2];
        s3 = in2[3];
        s4 = in2[4];

        t[0] = (crypto_wide)r0 * s0;
        t[1] = (crypto_wide)r0 * s1 + (crypto_wide)r1 * s0;
        t[2] = (crypto_wide)r0 * s2 + (crypto_wide)r2 * s0 + (crypto_wide)r1 * s1;
        t[3] = (crypto_wide)r0 * s3 + (crypto_wide)r3 * s0 + (crypto_wide)r1 * s2 +
               (crypto_wide)r2 * s1;
        t[4] = (crypto_wide)r0 * s4 + (crypto_wide)r4 * s0 + (crypto_wide)r3 * s1 +
               (crypto_wide)r1 * s3 + (crypto_wide)r2 * s2;

        t[0] += ((crypto_wide)r4 * 19) * s1 + ((crypto_wide)r1 * 19) * s4 +
                ((crypto_wide)r2 * 19) * s3 + ((crypto_wide)r3 * 19) * s2;
        t[1] += ((crypto_wide)r4 * 19) * s2 + ((crypto_wide)r2 * 19) * s4 +
                ((crypto_wide)r3 * 19) * s3;
        t[2] += ((crypto_wide)r4 * 19) * s3 + ((crypto_wide)r3 * 19) * s4;
        t[3] += ((crypto_wide)r4 * 19) * s4;

        r0 = (p64)t[0] & 0x7ffffffffffffull;
        c = (p64)(t[0] >> 51);
        t[1] += c;
        r1 = (p64)t[1] & 0x7ffffffffffffull;
        c = (p64)(t[1] >> 51);
        t[2] += c;
        r2 = (p64)t[2] & 0x7ffffffffffffull;
        c = (p64)(t[2] >> 51);
        t[3] += c;
        r3 = (p64)t[3] & 0x7ffffffffffffull;
        c = (p64)(t[3] >> 51);
        t[4] += c;
        r4 = (p64)t[4] & 0x7ffffffffffffull;
        c = (p64)(t[4] >> 51);
        r0 += c * 19;
        c = r0 >> 51;
        r0 &= 0x7ffffffffffffull;
        r1 += c;
        c = r1 >> 51;
        r1 &= 0x7ffffffffffffull;
        r2 += c;

        o[0] = r0;
        o[1] = r1;
        o[2] = r2;
        o[3] = r3;
        o[4] = r4;
        crypto_forget(t, sizeof t);
}

static fn crypto_x25519_sqr_n(crypto_x25519_fe o, crypto_x25519_fe a, positive n)
{
        crypto_x25519_fe t;

        crypto_x25519_mul(t, a, a);
        n--;
        while (n)
        {
                crypto_x25519_mul(t, t, t);
                n--;
        }
        crypto_x25519_copy(o, t);
        crypto_forget(t, sizeof t);
}

static fn crypto_x25519_mul121665(crypto_x25519_fe o, crypto_x25519_fe a)
{
        crypto_wide w;

        w = (crypto_wide)a[0] * 121665;
        o[0] = (p64)w & 0x7ffffffffffffull;
        w = (w >> 51) + (crypto_wide)a[1] * 121665;
        o[1] = (p64)w & 0x7ffffffffffffull;
        w = (w >> 51) + (crypto_wide)a[2] * 121665;
        o[2] = (p64)w & 0x7ffffffffffffull;
        w = (w >> 51) + (crypto_wide)a[3] * 121665;
        o[3] = (p64)w & 0x7ffffffffffffull;
        w = (w >> 51) + (crypto_wide)a[4] * 121665;
        o[4] = (p64)w & 0x7ffffffffffffull;
        o[0] += 19 * (p64)(w >> 51);
        crypto_forget(address_of w, sizeof w);
}

static fn crypto_x25519_invert(crypto_x25519_fe o, crypto_x25519_fe z)
{
        crypto_x25519_fe a, t0, b, c;

        crypto_x25519_sqr_n(a, z, 1);
        crypto_x25519_sqr_n(t0, a, 2);
        crypto_x25519_mul(b, t0, z);
        crypto_x25519_mul(a, b, a);
        crypto_x25519_sqr_n(t0, a, 1);
        crypto_x25519_mul(b, t0, b);
        crypto_x25519_sqr_n(t0, b, 5);
        crypto_x25519_mul(b, t0, b);
        crypto_x25519_sqr_n(t0, b, 10);
        crypto_x25519_mul(c, t0, b);
        crypto_x25519_sqr_n(t0, c, 20);
        crypto_x25519_mul(t0, t0, c);
        crypto_x25519_sqr_n(t0, t0, 10);
        crypto_x25519_mul(b, t0, b);
        crypto_x25519_sqr_n(t0, b, 50);
        crypto_x25519_mul(c, t0, b);
        crypto_x25519_sqr_n(t0, c, 100);
        crypto_x25519_mul(t0, t0, c);
        crypto_x25519_sqr_n(t0, t0, 50);
        crypto_x25519_mul(t0, t0, b);
        crypto_x25519_sqr_n(t0, t0, 5);
        crypto_x25519_mul(o, t0, a);

        crypto_forget(a, sizeof a);
        crypto_forget(t0, sizeof t0);
        crypto_forget(b, sizeof b);
        crypto_forget(c, sizeof c);
}

static fn crypto_cswap(crypto_x25519_fe a, crypto_x25519_fe b, p64 swap)
{
        positive i;

        swap = 0 - swap;
        for (i = 0; i < 5; i++)
        {
                p64 t = swap & (a[i] ^ b[i]);
                a[i] ^= t;
                b[i] ^= t;
        }
}

static bool crypto_x25519(p8 address_to out, p8 address_to scalar, p8 address_to u)
{
        p8 e[32];
        crypto_x25519_fe x1, x2, z2, x3, z3;
        crypto_x25519_fe a, b, c, d, aa, bb, ee, da, cb, t;
        positive i;
        p64 bit;
        p64 swap = 0;
        bool valid;

        memory_copy(e, scalar, 32);
        e[0] &= 248;
        e[31] &= 127;
        e[31] |= 64;

        crypto_x25519_load(x1, u);
        memory_fill(x2, 0, sizeof(x2));
        x2[0] = 1;
        memory_fill(z2, 0, sizeof(z2));
        crypto_x25519_copy(x3, x1);
        memory_fill(z3, 0, sizeof(z3));
        z3[0] = 1;

        for (i = 254; i < 256; i--)
        {
                bit = (e[i >> 3] >> (i & 7)) & 1;
                swap ^= bit;
                crypto_cswap(x2, x3, swap);
                crypto_cswap(z2, z3, swap);
                swap = bit;

                crypto_x25519_copy(a, x2);
                crypto_x25519_sum(a, z2);
                crypto_x25519_copy(b, z2);
                crypto_x25519_diff(b, x2);
                crypto_x25519_copy(c, x3);
                crypto_x25519_sum(c, z3);
                crypto_x25519_copy(d, z3);
                crypto_x25519_diff(d, x3);

                crypto_x25519_mul(da, d, a);
                crypto_x25519_mul(cb, c, b);
                crypto_x25519_mul(aa, a, a);
                crypto_x25519_mul(bb, b, b);

                crypto_x25519_copy(t, da);
                crypto_x25519_sum(t, cb);
                crypto_x25519_mul(x3, t, t);

                crypto_x25519_copy(t, cb);
                crypto_x25519_diff(t, da);
                crypto_x25519_mul(t, t, t);
                crypto_x25519_mul(z3, x1, t);

                crypto_x25519_mul(x2, aa, bb);

                crypto_x25519_copy(ee, bb);
                crypto_x25519_diff(ee, aa);
                crypto_x25519_mul121665(t, ee);
                crypto_x25519_sum(t, aa);
                crypto_x25519_mul(z2, ee, t);
        }

        crypto_cswap(x2, x3, swap);
        crypto_cswap(z2, z3, swap);
        crypto_x25519_invert(z2, z2);
        crypto_x25519_mul(x2, x2, z2);
        crypto_x25519_store(out, x2);

        /* RFC 7748's low-order inputs produce the all-zero shared secret.
           Returning its validity lets a protocol reject that public result
           without adding a second, easy-to-forget check at every caller. */
        {
                p8 nonzero = 0;

                for (i = 0; i < 32; i++)
                        nonzero |= out[i];
                valid = nonzero != 0;
                crypto_forget(address_of nonzero, sizeof nonzero);
        }

        crypto_forget(e, sizeof e);
        crypto_forget(x1, sizeof x1);
        crypto_forget(x2, sizeof x2);
        crypto_forget(z2, sizeof z2);
        crypto_forget(x3, sizeof x3);
        crypto_forget(z3, sizeof z3);
        crypto_forget(a, sizeof a);
        crypto_forget(b, sizeof b);
        crypto_forget(c, sizeof c);
        crypto_forget(d, sizeof d);
        crypto_forget(aa, sizeof aa);
        crypto_forget(bb, sizeof bb);
        crypto_forget(ee, sizeof ee);
        crypto_forget(da, sizeof da);
        crypto_forget(cb, sizeof cb);
        crypto_forget(t, sizeof t);
        crypto_forget(address_of bit, sizeof bit);
        crypto_forget(address_of swap, sizeof swap);
        return valid;
}

#define CRYPTO_FE_MAX 6
#define CRYPTO_RSA_LIMBS 64

static const p64 crypto_p256_p[4] = {
    0xffffffffffffffffull, 0x00000000ffffffffull, 0x0000000000000000ull,
    0xffffffff00000001ull};
static const p64 crypto_p256_n[4] = {
    0xf3b9cac2fc632551ull, 0xbce6faada7179e84ull, 0xffffffffffffffffull,
    0xffffffff00000000ull};
static const p8 crypto_p256_gx_be[32] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5,
    0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
    0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96};
static const p8 crypto_p256_gy_be[32] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a,
    0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
    0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};
static const p8 crypto_p256_b_be[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55,
    0x76, 0x98, 0x86, 0xbc, 0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6,
    0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b};

static const p8 crypto_p384_gx_be[48] = {
    0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37, 0x8e, 0xb1, 0xc7, 0x1e,
    0xf3, 0x20, 0xad, 0x74, 0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98,
    0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38, 0x55, 0x02, 0xf2, 0x5d,
    0xbf, 0x55, 0x29, 0x6c, 0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7};
static const p8 crypto_p384_gy_be[48] = {
    0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f, 0x5d, 0x9e, 0x98, 0xbf,
    0x92, 0x92, 0xdc, 0x29, 0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c,
    0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0, 0x0a, 0x60, 0xb1, 0xce,
    0x1d, 0x7e, 0x81, 0x9d, 0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f};
static const p8 crypto_p384_b_be[48] = {
    0xb3, 0x31, 0x2f, 0xa7, 0xe2, 0x3e, 0xe7, 0xe4, 0x98, 0x8e, 0x05, 0x6b,
    0xe3, 0xf8, 0x2d, 0x19, 0x18, 0x1d, 0x9c, 0x6e, 0xfe, 0x81, 0x41, 0x12,
    0x03, 0x14, 0x08, 0x8f, 0x50, 0x13, 0x87, 0x5a, 0xc6, 0x56, 0x39, 0x8d,
    0x8a, 0x2e, 0xd1, 0x9d, 0x2a, 0x85, 0xc8, 0xed, 0xd3, 0xec, 0x2a, 0xef};

static const p64 crypto_p384_p[6] = {
    0x00000000ffffffffull, 0xffffffff00000000ull, 0xfffffffffffffffeull,
    0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull};
static const p64 crypto_p384_n[6] = {
    0xecec196accc52973ull, 0x581a0db248b0a77aull, 0xc7634d81f4372ddfull,
    0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull};

/* Montgomery form.  An element a of Z/m is held as aR mod m, R = 2^(64n),
   so a product is reduced by n multiplies of its low limb by -1/m mod 2^64
   rather than by division.  Addition, subtraction, zero tests and equality
   read the same in either form; multiplication, the curve constant b and
   the edges where bytes come in or go out are where the form shows.  one is
   R mod m (the form of 1) and square is R^2 mod m (a*square reduces to aR).
   The constants were computed once from the moduli with exact integers. */
typedef struct
{
        positive n;
        p64 inverse;
        const p64 address_to m;
        const p64 address_to one;
        const p64 address_to square;
} crypto_field;

static const p64 crypto_p256_p_one[4] = {
    0x0000000000000001ull, 0xffffffff00000000ull, 0xffffffffffffffffull,
    0x00000000fffffffeull};
static const p64 crypto_p256_p_square[4] = {
    0x0000000000000003ull, 0xfffffffbffffffffull, 0xfffffffffffffffeull,
    0x00000004fffffffdull};
static const p64 crypto_p256_n_one[4] = {
    0x0c46353d039cdaafull, 0x4319055258e8617bull, 0x0000000000000000ull,
    0x00000000ffffffffull};
static const p64 crypto_p256_n_square[4] = {
    0x83244c95be79eea2ull, 0x4699799c49bd6fa6ull, 0x2845b2392b6bec59ull,
    0x66e12d94f3d95620ull};
static const p64 crypto_p384_p_one[6] = {
    0xffffffff00000001ull, 0x00000000ffffffffull, 0x0000000000000001ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull};
static const p64 crypto_p384_p_square[6] = {
    0xfffffffe00000001ull, 0x0000000200000000ull, 0xfffffffe00000000ull,
    0x0000000200000000ull, 0x0000000000000001ull, 0x0000000000000000ull};
static const p64 crypto_p384_n_one[6] = {
    0x1313e695333ad68dull, 0xa7e5f24db74f5885ull, 0x389cb27e0bc8d220ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull};
static const p64 crypto_p384_n_square[6] = {
    0x2d319b2419b409a9ull, 0xff3d81e5df1aa419ull, 0xbc3e483afcb82947ull,
    0xd40d49174aab1cc5ull, 0x3fb05b7a28266895ull, 0x0c84ee012b39bf21ull};

static const crypto_field crypto_p256_field = {
    4, 0x0000000000000001ull, crypto_p256_p, crypto_p256_p_one,
    crypto_p256_p_square};
static const crypto_field crypto_p256_order = {
    4, 0xccd1c8aaee00bc4full, crypto_p256_n, crypto_p256_n_one,
    crypto_p256_n_square};
static const crypto_field crypto_p384_field = {
    6, 0x0000000100000001ull, crypto_p384_p, crypto_p384_p_one,
    crypto_p384_p_square};
static const crypto_field crypto_p384_order = {
    6, 0x6ed46089e88fdc45ull, crypto_p384_n, crypto_p384_n_one,
    crypto_p384_n_square};

static fn crypto_fe_load_be(p64 address_to out, const p8 address_to bytes, positive n)
{
        positive i;

        for (i = 0; i < n; i++)
                out[n - 1 - i] = crypto_be64(bytes + i * 8);
}

static fn crypto_fe_store_be(p8 address_to bytes, const p64 address_to in, positive n)
{
        positive i;

        for (i = 0; i < n; i++)
                crypto_put_be64(bytes + i * 8, in[n - 1 - i]);
}

static bipolar crypto_fe_cmp(const p64 address_to a, const p64 address_to b,
                             positive n)
{
        positive i = n;

        while (i)
        {
                i--;
                if (a[i] > b[i])
                        return 1;
                if (a[i] < b[i])
                        return -1;
        }

        return 0;
}

/* Return the final borrow from a-b.  Unlike crypto_fe_cmp, this always walks
   every limb and is suitable for decisions derived from private points.
   d may be a. */
static p64 crypto_fe_subtract_raw(p64 address_to d,
                                  const p64 address_to a,
                                  const p64 address_to b, positive n)
{
        crypto_wide borrow = 0;

        for (positive i = 0; i < n; i++)
        {
                crypto_wide value = (crypto_wide)a[i] - b[i] - borrow;

                d[i] = (p64)value;
                borrow = (value >> 64) & 1;
        }

        return (p64)borrow;
}

static p64 crypto_fe_zero_bit(const p64 address_to a, positive n)
{
        p64 combined = 0;

        for (positive i = 0; i < n; i++)
                combined |= a[i];

        return ((combined | (0 - combined)) >> 63) ^ 1;
}

static fn crypto_fe_select(p64 address_to d, const p64 address_to a,
                           const p64 address_to b, positive n, p64 choose_b)
{
        p64 mask = 0 - choose_b;

        for (positive i = 0; i < n; i++)
                d[i] = (a[i] & ~mask) | (b[i] & mask);

        crypto_forget(address_of mask, sizeof mask);
}

static bool crypto_fe_is_zero(const p64 address_to a, positive n)
{
        return crypto_fe_zero_bit(a, n) != 0;
}

/* The two NIST field primes run on library.c's p256_ and p384_ routines;
   the C below serves any other modulus, and a crypto_field copied to
   another address, which is how CHECK_net compares the two. */
static fn crypto_fe_add(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const crypto_field address_to f)
{
        if (f == address_of crypto_p256_field)
        {
                p256_add(d, a, b);
                return;
        }
        if (f == address_of crypto_p384_field)
        {
                p384_add(d, a, b);
                return;
        }

        p64 sum[CRYPTO_FE_MAX];
        p64 reduced[CRYPTO_FE_MAX];
        crypto_wide carry = 0;
        positive n = f->n;
        p64 borrow;
        p64 reduce;

        for (positive i = 0; i < n; i++)
        {
                carry += (crypto_wide)a[i] + b[i];
                sum[i] = (p64)carry;
                carry >>= 64;
        }

        borrow = crypto_fe_subtract_raw(reduced, sum, f->m, n);
        reduce = (p64)carry | (borrow ^ 1);
        crypto_fe_select(d, sum, reduced, n, reduce);
        crypto_forget(sum, sizeof sum);
        crypto_forget(reduced, sizeof reduced);
        crypto_forget(address_of carry, sizeof carry);
        crypto_forget(address_of borrow, sizeof borrow);
        crypto_forget(address_of reduce, sizeof reduce);
}

static fn crypto_fe_sub(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const crypto_field address_to f)
{
        if (f == address_of crypto_p256_field)
        {
                p256_subtract(d, a, b);
                return;
        }
        if (f == address_of crypto_p384_field)
        {
                p384_subtract(d, a, b);
                return;
        }

        p64 difference[CRYPTO_FE_MAX];
        p64 restored[CRYPTO_FE_MAX];
        positive n = f->n;
        p64 borrow = crypto_fe_subtract_raw(difference, a, b, n);
        crypto_wide carry = 0;

        for (positive i = 0; i < n; i++)
        {
                carry += (crypto_wide)difference[i] + f->m[i];
                restored[i] = (p64)carry;
                carry >>= 64;
        }

        crypto_fe_select(d, difference, restored, n, borrow);
        crypto_forget(difference, sizeof difference);
        crypto_forget(restored, sizeof restored);
        crypto_forget(address_of borrow, sizeof borrow);
        crypto_forget(address_of carry, sizeof carry);
}

/* Montgomery reduction of the 2n-limb t, which it overwrites: d = t/R mod m.
   With t below mR the quotient left in the top half is below 2m, so one
   masked subtraction finishes it.  Every limb is visited whatever the
   values, and d may be an operand of the product t was made from. */
static fn crypto_montgomery_reduce(p64 address_to d, p64 address_to t,
                                   const p64 address_to m, p64 inverse,
                                   positive n)
{
        p64 reduced[CRYPTO_RSA_LIMBS];
        p64 top = 0;
        p64 borrow;

        for (positive i = 0; i < n; i++)
        {
                p64 q = t[i] * inverse;
                crypto_wide carry = 0;

                for (positive j = 0; j < n; j++)
                {
                        carry += (crypto_wide)t[i + j] + (crypto_wide)q * m[j];
                        t[i + j] = (p64)carry;
                        carry >>= 64;
                }
                carry += (crypto_wide)t[i + n] + top;
                t[i + n] = (p64)carry;
                top = (p64)(carry >> 64);
        }

        borrow = crypto_fe_subtract_raw(reduced, t + n, m, n);
        crypto_fe_select(d, t + n, reduced, n, top | (borrow ^ 1));
        crypto_forget(reduced, n * 8);
        crypto_forget(address_of top, sizeof top);
        crypto_forget(address_of borrow, sizeof borrow);
}

/* d = a*b/R mod m for a and b below m; d may alias either. */
static fn crypto_montgomery_multiply(p64 address_to d, const p64 address_to a,
                                     const p64 address_to b,
                                     const p64 address_to m, p64 inverse,
                                     positive n)
{
        p64 t[CRYPTO_RSA_LIMBS * 2];

        if (n > CRYPTO_RSA_LIMBS)
                return;

        memory_fill(t, 0, n * 2 * 8);
        for (positive i = 0; i < n; i++)
        {
                crypto_wide carry = 0;

                for (positive j = 0; j < n; j++)
                {
                        carry += (crypto_wide)t[i + j] +
                                 (crypto_wide)a[i] * b[j];
                        t[i + j] = (p64)carry;
                        carry >>= 64;
                }
                t[i + n] = (p64)carry;
        }

        crypto_montgomery_reduce(d, t, m, inverse, n);
        crypto_forget(t, n * 2 * 8);
}

/* d = a*a/R mod m.  Each cross product is made once and doubled with a
   shift before the diagonal squares are added, n(n-1)/2 multiplies fewer
   than the general product. */
static fn crypto_montgomery_square(p64 address_to d, const p64 address_to a,
                                   const p64 address_to m, p64 inverse,
                                   positive n)
{
        p64 t[CRYPTO_RSA_LIMBS * 2];
        crypto_wide carry = 0;

        if (n > CRYPTO_RSA_LIMBS)
                return;

        memory_fill(t, 0, n * 2 * 8);
        for (positive i = 0; i < n; i++)
        {
                carry = 0;
                for (positive j = i + 1; j < n; j++)
                {
                        carry += (crypto_wide)t[i + j] +
                                 (crypto_wide)a[i] * a[j];
                        t[i + j] = (p64)carry;
                        carry >>= 64;
                }
                t[i + n] = (p64)carry;
        }

        carry = 0;
        for (positive i = 0; i < n * 2; i++)
        {
                p64 limb = t[i];

                t[i] = (limb << 1) | (p64)carry;
                carry = limb >> 63;
        }

        carry = 0;
        for (positive i = 0; i < n; i++)
        {
                crypto_wide square = (crypto_wide)a[i] * a[i];

                carry += (crypto_wide)t[i * 2] + (p64)square;
                t[i * 2] = (p64)carry;
                carry >>= 64;
                carry += (crypto_wide)t[i * 2 + 1] + (p64)(square >> 64);
                t[i * 2 + 1] = (p64)carry;
                carry >>= 64;
        }

        crypto_montgomery_reduce(d, t, m, inverse, n);
        crypto_forget(t, n * 2 * 8);
        crypto_forget(address_of carry, sizeof carry);
}

static fn crypto_fe_mul(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const crypto_field address_to f)
{
        if (f == address_of crypto_p256_field)
                p256_multiply(d, a, b);
        else if (f == address_of crypto_p384_field)
                p384_multiply(d, a, b);
        else
                crypto_montgomery_multiply(d, a, b, f->m, f->inverse, f->n);
}

static fn crypto_fe_sqr(p64 address_to d, const p64 address_to a,
                        const crypto_field address_to f)
{
        if (f == address_of crypto_p256_field)
                p256_square(d, a);
        else if (f == address_of crypto_p384_field)
                p384_square(d, a);
        else
                crypto_montgomery_square(d, a, f->m, f->inverse, f->n);
}

/* d = 1/a in Montgomery form, by Fermat: a^(m-2).  The exponent is the
   public modulus, so its 4-bit digits choose which table entry multiplies
   in; the table holds a^0..a^15, and the squarings and multiplies follow
   the modulus alone whatever a is.  a = 0 gives 0. */
static fn crypto_fe_inv(p64 address_to d, const p64 address_to a,
                        const crypto_field address_to f)
{
        p64 table[16][CRYPTO_FE_MAX];
        p64 exponent[CRYPTO_FE_MAX];
        p64 result[CRYPTO_FE_MAX];
        p64 two[CRYPTO_FE_MAX];
        positive n = f->n;
        positive bit = n * 64;

        memory_fill(two, 0, sizeof two);
        two[0] = 2;
        crypto_fe_subtract_raw(exponent, f->m, two, n);
        memory_copy(table[0], f->one, n * 8);
        memory_copy(table[1], a, n * 8);
        for (positive i = 2; i < 16; i++)
                crypto_fe_mul(table[i], table[i - 1], a, f);

        memory_copy(result, f->one, n * 8);
        while (bit)
        {
                positive digit;

                bit -= 4;
                if (bit != n * 64 - 4)
                        for (positive i = 0; i < 4; i++)
                                crypto_fe_sqr(result, result, f);
                digit = (positive)(exponent[bit / 64] >> (bit % 64)) & 15;
                if (digit)
                        crypto_fe_mul(result, result, table[digit], f);
        }

        memory_copy(d, result, n * 8);
        crypto_forget(table, sizeof table);
        crypto_forget(exponent, sizeof exponent);
        crypto_forget(result, sizeof result);
}

/* Jacobian points: (X, Y, Z) is the affine (X/Z^2, Y/Z^3), coordinates in
   the field's Montgomery form, and Z = 0 is infinity.  crypto_point_affine
   is the exit: it leaves x and y as plain integers with z = 1, for output
   and comparison only. */
typedef struct
{
        p64 x[CRYPTO_FE_MAX];
        p64 y[CRYPTO_FE_MAX];
        p64 z[CRYPTO_FE_MAX];
        positive n;
        const crypto_field address_to field;
} crypto_point;

static fn crypto_point_zero(crypto_point address_to q,
                            const crypto_field address_to f)
{
        memory_fill(q, 0, sizeof(*q));
        q->n = f->n;
        q->field = f;
}

/* x and y are plain integers below the field modulus. */
static fn crypto_point_set_xy(crypto_point address_to q, const p64 address_to x,
                              const p64 address_to y,
                              const crypto_field address_to f)
{
        crypto_point_zero(q, f);
        crypto_fe_mul(q->x, x, f->square, f);
        crypto_fe_mul(q->y, y, f->square, f);
        memory_copy(q->z, f->one, f->n * 8);
}

/* dbl-2001-b for a = -3, three multiplies and five squarings:
       delta = Z^2, gamma = Y^2, beta = X gamma,
       alpha = 3 (X - delta)(X + delta),
       X3 = alpha^2 - 8 beta,  Z3 = (Y + Z)^2 - gamma - delta,
       Y3 = alpha (4 beta - X3) - 8 gamma^2.
   Z = 0 gives Z3 = 0, so infinity doubles to itself with no branch.  The
   same operations run for every input, and r may be p. */
static fn crypto_point_double_formula(crypto_point address_to r,
                                      const crypto_point address_to p)
{
        const crypto_field address_to f = p->field;
        p64 delta[CRYPTO_FE_MAX], gamma[CRYPTO_FE_MAX], beta[CRYPTO_FE_MAX];
        p64 alpha[CRYPTO_FE_MAX], tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        p64 x3[CRYPTO_FE_MAX], y3[CRYPTO_FE_MAX], z3[CRYPTO_FE_MAX];

        crypto_fe_sqr(delta, p->z, f);
        crypto_fe_sqr(gamma, p->y, f);
        crypto_fe_mul(beta, p->x, gamma, f);

        crypto_fe_sub(tmp, p->x, delta, f);
        crypto_fe_add(tmp2, p->x, delta, f);
        crypto_fe_mul(alpha, tmp, tmp2, f);
        crypto_fe_add(tmp, alpha, alpha, f);
        crypto_fe_add(alpha, tmp, alpha, f);

        crypto_fe_add(z3, p->y, p->z, f);
        crypto_fe_sqr(z3, z3, f);
        crypto_fe_sub(z3, z3, gamma, f);
        crypto_fe_sub(z3, z3, delta, f);

        crypto_fe_add(beta, beta, beta, f);
        crypto_fe_add(beta, beta, beta, f);
        crypto_fe_sqr(x3, alpha, f);
        crypto_fe_add(tmp, beta, beta, f);
        crypto_fe_sub(x3, x3, tmp, f);

        crypto_fe_sub(tmp, beta, x3, f);
        crypto_fe_mul(y3, alpha, tmp, f);
        crypto_fe_sqr(tmp, gamma, f);
        crypto_fe_add(tmp, tmp, tmp, f);
        crypto_fe_add(tmp, tmp, tmp, f);
        crypto_fe_add(tmp, tmp, tmp, f);
        crypto_fe_sub(y3, y3, tmp, f);

        memory_copy(r->x, x3, sizeof x3);
        memory_copy(r->y, y3, sizeof y3);
        memory_copy(r->z, z3, sizeof z3);
        r->n = p->n;
        r->field = f;

        crypto_forget(delta, sizeof delta);
        crypto_forget(gamma, sizeof gamma);
        crypto_forget(beta, sizeof beta);
        crypto_forget(alpha, sizeof alpha);
        crypto_forget(tmp, sizeof tmp);
        crypto_forget(tmp2, sizeof tmp2);
        crypto_forget(x3, sizeof x3);
        crypto_forget(y3, sizeof y3);
        crypto_forget(z3, sizeof z3);
}

/* Public points only: infinity returns at once. */
static fn crypto_point_double(crypto_point address_to r, crypto_point address_to p)
{
        if (crypto_fe_is_zero(p->z, p->n))
        {
                *r = *p;
                return;
        }
        crypto_point_double_formula(r, p);
}

static fn crypto_point_add(crypto_point address_to r, crypto_point address_to p,
                           crypto_point address_to q)
{
        p64 z1z1[CRYPTO_FE_MAX], z2z2[CRYPTO_FE_MAX];
        p64 u1[CRYPTO_FE_MAX], u2[CRYPTO_FE_MAX], s1[CRYPTO_FE_MAX], s2[CRYPTO_FE_MAX];
        p64 h[CRYPTO_FE_MAX], rr[CRYPTO_FE_MAX], hh[CRYPTO_FE_MAX], hhh[CRYPTO_FE_MAX];
        p64 v[CRYPTO_FE_MAX], tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        positive n = p->n;
        const crypto_field address_to f = p->field;

        if (crypto_fe_is_zero(p->z, n))
        {
                *r = *q;
                return;
        }
        if (crypto_fe_is_zero(q->z, n))
        {
                *r = *p;
                return;
        }

        crypto_fe_sqr(z1z1, p->z, f);
        crypto_fe_sqr(z2z2, q->z, f);
        crypto_fe_mul(u1, p->x, z2z2, f);
        crypto_fe_mul(u2, q->x, z1z1, f);
        crypto_fe_mul(tmp, q->z, z2z2, f);
        crypto_fe_mul(s1, p->y, tmp, f);
        crypto_fe_mul(tmp, p->z, z1z1, f);
        crypto_fe_mul(s2, q->y, tmp, f);

        crypto_fe_sub(h, u2, u1, f);
        crypto_fe_sub(rr, s2, s1, f);

        if (crypto_fe_is_zero(h, n))
        {
                if (crypto_fe_is_zero(rr, n))
                {
                        crypto_point_double(r, p);
                        return;
                }
                crypto_point_zero(r, f);
                return;
        }

        crypto_fe_sqr(hh, h, f);
        crypto_fe_mul(hhh, h, hh, f);
        crypto_fe_mul(v, u1, hh, f);

        crypto_fe_sqr(tmp, rr, f);
        crypto_fe_sub(tmp, tmp, hhh, f);
        crypto_fe_add(tmp2, v, v, f);
        crypto_fe_sub(r->x, tmp, tmp2, f);

        crypto_fe_sub(tmp, v, r->x, f);
        crypto_fe_mul(tmp2, rr, tmp, f);
        crypto_fe_mul(tmp, s1, hhh, f);
        crypto_fe_sub(r->y, tmp2, tmp, f);

        crypto_fe_mul(tmp, p->z, q->z, f);
        crypto_fe_mul(r->z, tmp, h, f);
        r->n = n;
        r->field = f;
}

/* The ECDH multiplier cannot use the public-signature helpers above: their
   exceptional-point branches and digit-conditional addition reveal a private
   scalar to a branch or cache observer.  These helpers select infinity cases
   with masks, and crypto_point_scalar_private says why no other exception
   reaches them.  Field reduction is likewise branchless, so every scalar
   follows the same operations and addresses. */
static fn crypto_point_select(crypto_point address_to d,
                              const crypto_point address_to a,
                              const crypto_point address_to b, p64 choose_b)
{
        p64 mask = 0 - choose_b;

        for (positive i = 0; i < CRYPTO_FE_MAX; i++)
        {
                d->x[i] = (a->x[i] & ~mask) | (b->x[i] & mask);
                d->y[i] = (a->y[i] & ~mask) | (b->y[i] & mask);
                d->z[i] = (a->z[i] & ~mask) | (b->z[i] & mask);
        }
        d->n = a->n;
        d->field = a->field;
        crypto_forget(address_of mask, sizeof mask);
}

static fn crypto_point_double_private(crypto_point address_to r,
                                      const crypto_point address_to p)
{
        crypto_point_double_formula(r, p);
}

static fn crypto_point_add_private(crypto_point address_to r,
                                   const crypto_point address_to p,
                                   const crypto_point address_to q)
{
        p64 z1z1[CRYPTO_FE_MAX], z2z2[CRYPTO_FE_MAX];
        p64 u1[CRYPTO_FE_MAX], u2[CRYPTO_FE_MAX];
        p64 s1[CRYPTO_FE_MAX], s2[CRYPTO_FE_MAX];
        p64 h[CRYPTO_FE_MAX], rr[CRYPTO_FE_MAX], hh[CRYPTO_FE_MAX];
        p64 hhh[CRYPTO_FE_MAX], v[CRYPTO_FE_MAX];
        p64 tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        positive n = p->n;
        const crypto_field address_to f = p->field;
        crypto_point sum;
        p64 p_infinity;
        p64 q_infinity;

        crypto_point_zero(address_of sum, f);
        crypto_fe_sqr(z1z1, p->z, f);
        crypto_fe_sqr(z2z2, q->z, f);
        crypto_fe_mul(u1, p->x, z2z2, f);
        crypto_fe_mul(u2, q->x, z1z1, f);
        crypto_fe_mul(tmp, q->z, z2z2, f);
        crypto_fe_mul(s1, p->y, tmp, f);
        crypto_fe_mul(tmp, p->z, z1z1, f);
        crypto_fe_mul(s2, q->y, tmp, f);

        crypto_fe_sub(h, u2, u1, f);
        crypto_fe_sub(rr, s2, s1, f);
        crypto_fe_sqr(hh, h, f);
        crypto_fe_mul(hhh, h, hh, f);
        crypto_fe_mul(v, u1, hh, f);

        crypto_fe_sqr(tmp, rr, f);
        crypto_fe_sub(tmp, tmp, hhh, f);
        crypto_fe_add(tmp2, v, v, f);
        crypto_fe_sub(sum.x, tmp, tmp2, f);

        crypto_fe_sub(tmp, v, sum.x, f);
        crypto_fe_mul(tmp2, rr, tmp, f);
        crypto_fe_mul(tmp, s1, hhh, f);
        crypto_fe_sub(sum.y, tmp2, tmp, f);

        crypto_fe_mul(tmp, p->z, q->z, f);
        crypto_fe_mul(sum.z, tmp, h, f);

        /* The window multiplier never adds equal points (see
           crypto_point_scalar_private), and opposite points already produce
           z=0.  Only the infinity cases need masked selection.  r may be
           p: nothing is written through it before the last line. */
        p_infinity = crypto_fe_zero_bit(p->z, n);
        q_infinity = crypto_fe_zero_bit(q->z, n);
        crypto_point_select(address_of sum, address_of sum, p, q_infinity);
        crypto_point_select(address_of sum, address_of sum, q, p_infinity);
        *r = sum;

        crypto_forget(z1z1, sizeof z1z1);
        crypto_forget(z2z2, sizeof z2z2);
        crypto_forget(u1, sizeof u1);
        crypto_forget(u2, sizeof u2);
        crypto_forget(s1, sizeof s1);
        crypto_forget(s2, sizeof s2);
        crypto_forget(h, sizeof h);
        crypto_forget(rr, sizeof rr);
        crypto_forget(hh, sizeof hh);
        crypto_forget(hhh, sizeof hhh);
        crypto_forget(v, sizeof v);
        crypto_forget(tmp, sizeof tmp);
        crypto_forget(tmp2, sizeof tmp2);
        crypto_forget(address_of sum, sizeof sum);
        crypto_forget(address_of p_infinity, sizeof p_infinity);
        crypto_forget(address_of q_infinity, sizeof q_infinity);
}

typedef struct
{
        positive bits;
        positive point_adds;
        positive point_doubles;
        positive conditional_swaps;
        positive conditional_selects;
} crypto_scalar_schedule;

/* k*P for a private k below the group order n, by 4-bit fixed windows.
   The table 0P..15P comes from the public P alone; each window then doubles
   four times and adds the entry its digit names, found by a masked scan of
   all sixteen, so the operations, their order and every address are the
   same for every k.  crypto_point_add_private selects its infinity cases
   with masks, and no other exception can arise: before a window's addition
   the accumulator is K*P, K being the integer value of k's higher windows
   times 16, and the entry is d*P with d below 16.  K = d (mod n) with
   K <= k < n holds only for K = d = 0, both infinity, and K = -d (mod n)
   would need K + d = n, above k.  The table's own additions are (j-1)P + P
   for j from 3, never equal points. */
static fn crypto_point_scalar_private(
    crypto_point address_to r, const crypto_point address_to p,
    const p64 address_to k, crypto_scalar_schedule address_to schedule)
{
        crypto_point table[16];
        crypto_point accumulator;
        crypto_point chosen;
        positive bits = p->n * 64;
        p64 digit = 0;

        if (schedule)
                memory_fill(schedule, 0, sizeof(*schedule));

        crypto_point_zero(address_of table[0], p->field);
        table[1] = *p;
        crypto_point_double_private(address_of table[2], p);
        for (positive j = 3; j < 16; j++)
                crypto_point_add_private(address_of table[j],
                                         address_of table[j - 1], p);
        if (schedule)
        {
                schedule->point_doubles = 1;
                schedule->point_adds = 13;
                schedule->conditional_selects = 13 * 2;
        }

        crypto_point_zero(address_of accumulator, p->field);
        for (positive at = bits; at;)
        {
                at -= 4;
                if (at != bits - 4)
                        for (positive i = 0; i < 4; i++)
                                crypto_point_double_private(
                                    address_of accumulator,
                                    address_of accumulator);
                digit = (k[at / 64] >> (at % 64)) & 15;
                chosen = table[0];
                for (p64 j = 1; j < 16; j++)
                        crypto_point_select(address_of chosen,
                                            address_of chosen,
                                            address_of table[j],
                                            ((j ^ digit) - 1) >> 63);
                crypto_point_add_private(address_of accumulator,
                                         address_of accumulator,
                                         address_of chosen);

                if (schedule)
                {
                        schedule->bits += 4;
                        if (at != bits - 4)
                                schedule->point_doubles += 4;
                        schedule->point_adds++;
                        schedule->conditional_selects += 15 + 2;
                }
        }

        *r = accumulator;
        crypto_forget(table, sizeof table);
        crypto_forget(address_of accumulator, sizeof accumulator);
        crypto_forget(address_of chosen, sizeof chosen);
        crypto_forget(address_of digit, sizeof digit);
}

/* Width-5 non-adjacent form of a public scalar below 2^(64 limbs): each
   digit is zero or odd in [-15, 15], a nonzero digit is followed by at least
   four zeros, and the digits weighted by 2^i sum to k.  Returns how many
   digits were written, at most 64 limbs + 1. */
static positive crypto_wnaf(b8 address_to digits, const p64 address_to k,
                            positive limbs)
{
        p64 v[CRYPTO_FE_MAX + 1];
        positive count = 0;

        memory_copy(v, k, limbs * 8);
        v[limbs] = 0;
        while (!crypto_fe_is_zero(v, limbs + 1))
        {
                b8 digit = 0;

                if (v[0] & 1)
                {
                        p64 low = v[0] & 31;

                        if (low > 15)
                        {
                                p64 carry = 32 - low;

                                digit = (b8)((bipolar)low - 32);
                                for (positive i = 0; carry && i <= limbs; i++)
                                {
                                        v[i] += carry;
                                        carry = v[i] < carry;
                                }
                        }
                        else
                        {
                                digit = (b8)low;
                                v[0] -= low;
                        }
                }
                digits[count++] = digit;
                for (positive i = 0; i < limbs; i++)
                        v[i] = (v[i] >> 1) | (v[i + 1] << 63);
                v[limbs] >>= 1;
        }

        return count;
}

/* table[j] = (2j + 1) P for j below 8. */
static fn crypto_point_odd_multiples(crypto_point address_to table,
                                     crypto_point address_to p)
{
        crypto_point twice;

        table[0] = *p;
        crypto_point_double(address_of twice, p);
        for (positive j = 1; j < 8; j++)
                crypto_point_add(address_of table[j], address_of table[j - 1],
                                 address_of twice);
}

static fn crypto_point_add_digit(crypto_point address_to r,
                                 const crypto_point address_to table, b8 digit)
{
        crypto_point entry;
        crypto_point sum;

        if (digit > 0)
                entry = table[digit >> 1];
        else
        {
                p64 zero[CRYPTO_FE_MAX];

                memory_fill(zero, 0, sizeof zero);
                entry = table[(-digit) >> 1];
                crypto_fe_sub(entry.y, zero, entry.y, entry.field);
        }
        crypto_point_add(address_of sum, r, address_of entry);
        *r = sum;
}

/* u1 G + u2 Q for public scalars and points (below 2^(64 limbs)), in one
   chain of doublings: each scalar's width-5 digits add a precomputed odd
   multiple of its own point or its negation.  About bits doublings and
   bits/3 additions against bits doublings and bits additions for two
   separate binary multiplies.  Everything may branch; nothing is secret. */
static fn crypto_point_double_scalar(crypto_point address_to r,
                                     crypto_point address_to g,
                                     const p64 address_to u1,
                                     crypto_point address_to q,
                                     const p64 address_to u2)
{
        b8 d1[CRYPTO_FE_MAX * 64 + 1];
        b8 d2[CRYPTO_FE_MAX * 64 + 1];
        crypto_point tg[8];
        crypto_point tq[8];
        crypto_point doubled;
        positive n1 = crypto_wnaf(d1, u1, g->n);
        positive n2 = crypto_wnaf(d2, u2, g->n);
        positive at = n1 > n2 ? n1 : n2;

        crypto_point_zero(r, g->field);
        if (n1)
                crypto_point_odd_multiples(tg, g);
        if (n2)
                crypto_point_odd_multiples(tq, q);
        while (at)
        {
                at--;
                crypto_point_double(address_of doubled, r);
                *r = doubled;
                if (at < n1 && d1[at])
                        crypto_point_add_digit(r, tg, d1[at]);
                if (at < n2 && d2[at])
                        crypto_point_add_digit(r, tq, d2[at]);
        }
}

static fn crypto_point_affine(crypto_point address_to p)
{
        const crypto_field address_to f = p->field;
        p64 zinv[CRYPTO_FE_MAX], z2[CRYPTO_FE_MAX], z3[CRYPTO_FE_MAX];
        p64 unit[CRYPTO_FE_MAX];

        if (crypto_fe_is_zero(p->z, p->n))
                goto done;

        crypto_fe_inv(zinv, p->z, f);
        crypto_fe_sqr(z2, zinv, f);
        crypto_fe_mul(z3, z2, zinv, f);
        crypto_fe_mul(p->x, p->x, z2, f);
        crypto_fe_mul(p->y, p->y, z3, f);
        memory_fill(unit, 0, sizeof unit);
        unit[0] = 1;
        crypto_fe_mul(p->x, p->x, unit, f);
        crypto_fe_mul(p->y, p->y, unit, f);
        memory_fill(p->z, 0, sizeof p->z);
        p->z[0] = 1;

done:
        crypto_forget(zinv, sizeof zinv);
        crypto_forget(z2, sizeof z2);
        crypto_forget(z3, sizeof z3);
}

static bool crypto_scalar_from_int_be(p64 address_to out,
                                      const p8 address_to bytes,
                                      positive length, const p64 address_to n,
                                      positive limbs)
{
        p8 padded[48];
        p64 difference[CRYPTO_FE_MAX];
        p64 less;
        bool valid;

        if (!length || length > limbs * 8)
                return false;
        memory_fill(out, 0, limbs * 8);
        memory_fill(padded, 0, sizeof(padded));
        memory_copy(padded + limbs * 8 - length, bytes, length);
        crypto_fe_load_be(out, padded, limbs);
        less = crypto_fe_subtract_raw(difference, out, n, limbs);
        valid = !crypto_fe_is_zero(out, limbs) && less;
        crypto_forget(padded, sizeof padded);
        crypto_forget(difference, sizeof difference);
        crypto_forget(address_of less, sizeof less);
        return valid;
}

static bool crypto_point_is_on_curve(const p8 address_to x_bytes,
                                     const p8 address_to y_bytes,
                                     const crypto_field address_to f,
                                     const p8 address_to b_bytes)
{
        p64 x[CRYPTO_FE_MAX], y[CRYPTO_FE_MAX], b[CRYPTO_FE_MAX];
        p64 left[CRYPTO_FE_MAX], right[CRYPTO_FE_MAX];
        p64 x2[CRYPTO_FE_MAX];
        positive limbs = f->n;

        crypto_fe_load_be(x, x_bytes, limbs);
        crypto_fe_load_be(y, y_bytes, limbs);
        crypto_fe_load_be(b, b_bytes, limbs);
        if (crypto_fe_cmp(x, f->m, limbs) >= 0 ||
            crypto_fe_cmp(y, f->m, limbs) >= 0)
                return false;

        /* NIST P-256 and P-384 both use y^2 = x^3 - 3x + b.  Affine
           infinity has no encoding, and (0,0) fails this equation.  Both
           sides are compared in Montgomery form, where equality is still
           equality. */
        crypto_fe_mul(x, x, f->square, f);
        crypto_fe_mul(y, y, f->square, f);
        crypto_fe_mul(b, b, f->square, f);
        crypto_fe_sqr(left, y, f);
        crypto_fe_sqr(x2, x, f);
        crypto_fe_mul(right, x2, x, f);
        crypto_fe_sub(right, right, x, f);
        crypto_fe_sub(right, right, x, f);
        crypto_fe_sub(right, right, x, f);
        crypto_fe_add(right, right, b, f);

        return crypto_fe_cmp(left, right, limbs) == 0;
}

/* Everything here is public: the key, the signature and the digest.  The
   scalar multiplies may therefore branch on their bits. */
static bool crypto_ecdsa_verify(p8 address_to hash, positive hash_length,
                                p8 address_to r_bytes, positive r_length,
                                p8 address_to s_bytes, positive s_length,
                                p8 address_to qx, p8 address_to qy,
                                const crypto_field address_to field,
                                const crypto_field address_to order,
                                const p8 address_to gx, const p8 address_to gy,
                                const p8 address_to b)
{
        p64 r[CRYPTO_FE_MAX], s[CRYPTO_FE_MAX], e[CRYPTO_FE_MAX];
        p64 w[CRYPTO_FE_MAX], u1[CRYPTO_FE_MAX], u2[CRYPTO_FE_MAX];
        p64 gx_f[CRYPTO_FE_MAX], gy_f[CRYPTO_FE_MAX], qx_f[CRYPTO_FE_MAX],
            qy_f[CRYPTO_FE_MAX];
        crypto_point g, q, rpoint;
        positive limbs = field->n;
        p8 ehash[48];

        if (!crypto_scalar_from_int_be(r, r_bytes, r_length, order->m, limbs) ||
            !crypto_scalar_from_int_be(s, s_bytes, s_length, order->m, limbs) ||
            !crypto_point_is_on_curve(qx, qy, field, b))
                return false;

        memory_fill(ehash, 0, sizeof(ehash));
        if (hash_length >= limbs * 8)
                memory_copy(ehash, hash, limbs * 8);
        else
                memory_copy(ehash + limbs * 8 - hash_length, hash, hash_length);
        crypto_fe_load_be(e, ehash, limbs);
        while (crypto_fe_cmp(e, order->m, limbs) >= 0)
                crypto_fe_subtract_raw(e, e, order->m, limbs);

        /* w = 1/s in Montgomery form, so multiplying a plain e or r by it
           reduces straight to the plain e/s and r/s. */
        crypto_fe_mul(w, s, order->square, order);
        crypto_fe_inv(w, w, order);
        crypto_fe_mul(u1, e, w, order);
        crypto_fe_mul(u2, r, w, order);

        crypto_fe_load_be(gx_f, gx, limbs);
        crypto_fe_load_be(gy_f, gy, limbs);
        crypto_fe_load_be(qx_f, qx, limbs);
        crypto_fe_load_be(qy_f, qy, limbs);

        crypto_point_set_xy(address_of g, gx_f, gy_f, field);
        crypto_point_set_xy(address_of q, qx_f, qy_f, field);
        crypto_point_double_scalar(address_of rpoint, address_of g, u1,
                                   address_of q, u2);
        if (crypto_fe_is_zero(rpoint.z, limbs))
                return false;
        crypto_point_affine(address_of rpoint);
        while (crypto_fe_cmp(rpoint.x, order->m, limbs) >= 0)
                crypto_fe_subtract_raw(rpoint.x, rpoint.x, order->m, limbs);

        return crypto_fe_cmp(rpoint.x, r, limbs) == 0;
}

static bool crypto_ecdsa_p256(p8 address_to hash, positive hash_length,
                              p8 address_to r, positive r_length, p8 address_to s,
                              positive s_length, p8 address_to qx, p8 address_to qy)
{
        return crypto_ecdsa_verify(hash, hash_length, r, r_length, s, s_length,
                                   qx, qy, address_of crypto_p256_field,
                                   address_of crypto_p256_order,
                                   crypto_p256_gx_be, crypto_p256_gy_be,
                                   crypto_p256_b_be);
}

static bool crypto_ecdsa_p384(p8 address_to hash, positive hash_length,
                              p8 address_to r, positive r_length, p8 address_to s,
                              positive s_length, p8 address_to qx, p8 address_to qy)
{
        return crypto_ecdsa_verify(hash, hash_length, r, r_length, s, s_length,
                                   qx, qy, address_of crypto_p384_field,
                                   address_of crypto_p384_order,
                                   crypto_p384_gx_be, crypto_p384_gy_be,
                                   crypto_p384_b_be);
}

static bool crypto_scalar_reduce_be(p8 address_to out, const p8 address_to bytes,
                                    positive length, const p64 address_to order,
                                    positive limbs)
{
        p64 k[CRYPTO_FE_MAX];
        p64 reduced[CRYPTO_FE_MAX];
        p64 less;

        if (length != limbs * 8)
                return false;

        crypto_fe_load_be(k, bytes, limbs);
        less = crypto_fe_subtract_raw(reduced, k, order, limbs);
        crypto_fe_select(k, reduced, k, limbs, less);
        if (crypto_fe_is_zero(k, limbs))
        {
                crypto_forget(k, sizeof(k));
                crypto_forget(reduced, sizeof reduced);
                crypto_forget(address_of less, sizeof less);
                return false;
        }

        crypto_fe_store_be(out, k, limbs);
        crypto_forget(k, sizeof(k));
        crypto_forget(reduced, sizeof reduced);
        crypto_forget(address_of less, sizeof less);
        return true;
}

static bool crypto_ecdh_public(p8 address_to out, p8 address_to scalar,
                               const crypto_field address_to field,
                               const crypto_field address_to order,
                               const p8 address_to gx, const p8 address_to gy)
{
        p64 k[CRYPTO_FE_MAX];
        p64 gx_f[CRYPTO_FE_MAX];
        p64 gy_f[CRYPTO_FE_MAX];
        crypto_point g;
        crypto_point r;
        positive limbs = field->n;
        bool ok = false;

        if (!crypto_scalar_from_int_be(k, scalar, limbs * 8, order->m, limbs))
                goto done;

        crypto_fe_load_be(gx_f, gx, limbs);
        crypto_fe_load_be(gy_f, gy, limbs);
        crypto_point_set_xy(address_of g, gx_f, gy_f, field);
        crypto_point_scalar_private(address_of r, address_of g, k, null);
        if (crypto_fe_is_zero(r.z, limbs))
                goto done;

        crypto_point_affine(address_of r);
        out[0] = 4;
        crypto_fe_store_be(out + 1, r.x, limbs);
        crypto_fe_store_be(out + 1 + limbs * 8, r.y, limbs);
        ok = true;

done:
        crypto_forget(k, sizeof(k));
        crypto_forget(gx_f, sizeof(gx_f));
        crypto_forget(gy_f, sizeof(gy_f));
        crypto_forget(address_of g, sizeof(g));
        crypto_forget(address_of r, sizeof(r));
        return ok;
}

static bool crypto_ecdh_shared(p8 address_to out, p8 address_to scalar,
                               p8 address_to peer, positive peer_length,
                               const crypto_field address_to field,
                               const crypto_field address_to order,
                               const p8 address_to b)
{
        p64 k[CRYPTO_FE_MAX];
        p64 qx[CRYPTO_FE_MAX];
        p64 qy[CRYPTO_FE_MAX];
        crypto_point q;
        crypto_point r;
        positive limbs = field->n;
        positive coord = limbs * 8;
        bool ok = false;

        if (peer_length != 1 + 2 * coord || peer[0] != 4 ||
            !crypto_point_is_on_curve(peer + 1, peer + 1 + coord, field, b) ||
            !crypto_scalar_from_int_be(k, scalar, coord, order->m, limbs))
                goto done;

        crypto_fe_load_be(qx, peer + 1, limbs);
        crypto_fe_load_be(qy, peer + 1 + coord, limbs);
        crypto_point_set_xy(address_of q, qx, qy, field);
        crypto_point_scalar_private(address_of r, address_of q, k, null);
        if (crypto_fe_is_zero(r.z, limbs))
                goto done;

        crypto_point_affine(address_of r);
        crypto_fe_store_be(out, r.x, limbs);
        ok = true;

done:
        crypto_forget(k, sizeof(k));
        crypto_forget(qx, sizeof(qx));
        crypto_forget(qy, sizeof(qy));
        crypto_forget(address_of q, sizeof(q));
        crypto_forget(address_of r, sizeof(r));
        return ok;
}

static bool crypto_ecdh_p256_public(p8 address_to out, p8 address_to scalar)
{
        return crypto_ecdh_public(out, scalar, address_of crypto_p256_field,
                                  address_of crypto_p256_order,
                                  crypto_p256_gx_be, crypto_p256_gy_be);
}

static bool crypto_ecdh_p256_shared(p8 address_to out, p8 address_to scalar,
                                    p8 address_to peer)
{
        return crypto_ecdh_shared(out, scalar, peer, 65,
                                  address_of crypto_p256_field,
                                  address_of crypto_p256_order,
                                  crypto_p256_b_be);
}

static bool crypto_ecdh_p384_public(p8 address_to out, p8 address_to scalar)
{
        return crypto_ecdh_public(out, scalar, address_of crypto_p384_field,
                                  address_of crypto_p384_order,
                                  crypto_p384_gx_be, crypto_p384_gy_be);
}

static bool crypto_ecdh_p384_shared(p8 address_to out, p8 address_to scalar,
                                    p8 address_to peer)
{
        return crypto_ecdh_shared(out, scalar, peer, 97,
                                  address_of crypto_p384_field,
                                  address_of crypto_p384_order,
                                  crypto_p384_b_be);
}

/* x = 2x mod m for x below m.  Public moduli only: the reduction branches. */
static fn crypto_rsa_double(p64 address_to x, const p64 address_to m, positive n)
{
        p64 carry = 0;

        for (positive i = 0; i < n; i++)
        {
                p64 limb = x[i];

                x[i] = (limb << 1) | carry;
                carry = limb >> 63;
        }
        if (carry || crypto_fe_cmp(x, m, n) >= 0)
                crypto_fe_subtract_raw(x, x, m, n);
}

/* out = base^exp mod m, for a public odd m of n limbs whose top limb is
   nonzero and base below m, in Montgomery form throughout.  -1/m mod 2^64
   comes by Newton's iteration: an odd m0 is its own inverse to three bits
   and each step doubles the correct bits.  R mod m starts from the top bit
   of m, which is below m, and doubles up to 2^(64n).  R^2 mod m is then the
   Montgomery form of 2^(64n): k doublings of the form of 1 make the form of
   2^k, and s squarings the form of 2^(k 2^s), with k 2^s = 64n. */
static fn crypto_rsa_modexp(p64 address_to out, p64 address_to base, p64 exp,
                            p64 address_to mod, positive n)
{
        p64 one[CRYPTO_RSA_LIMBS];
        p64 square[CRYPTO_RSA_LIMBS];
        p64 b[CRYPTO_RSA_LIMBS];
        p64 result[CRYPTO_RSA_LIMBS];
        p64 unit[CRYPTO_RSA_LIMBS];
        p64 inverse = mod[0];
        positive bits;
        positive top;
        positive k;
        positive s = 0;
        positive at;

        memory_fill(out, 0, CRYPTO_RSA_LIMBS * sizeof(p64));
        if (!n || n > CRYPTO_RSA_LIMBS || !mod[n - 1] || !(mod[0] & 1) || !exp)
                return;

        for (positive i = 0; i < 5; i++)
                inverse *= 2 - mod[0] * inverse;
        inverse = 0 - inverse;

        top = 63;
        while (!((mod[n - 1] >> top) & 1))
                top--;
        bits = (n - 1) * 64 + top + 1;
        memory_fill(one, 0, n * 8);
        one[n - 1] = (p64)1 << top;
        for (at = bits - 1; at < n * 64; at++)
                crypto_rsa_double(one, mod, n);

        k = n * 64;
        while (!(k & 1))
        {
                k >>= 1;
                s++;
        }
        memory_copy(square, one, n * 8);
        for (at = 0; at < k; at++)
                crypto_rsa_double(square, mod, n);
        for (at = 0; at < s; at++)
                crypto_montgomery_square(square, square, mod, inverse, n);

        crypto_montgomery_multiply(b, base, square, mod, inverse, n);
        top = 63;
        while (!((exp >> top) & 1))
                top--;
        memory_copy(result, b, n * 8);
        while (top)
        {
                top--;
                crypto_montgomery_square(result, result, mod, inverse, n);
                if ((exp >> top) & 1)
                        crypto_montgomery_multiply(result, result, b, mod,
                                                   inverse, n);
        }

        memory_fill(unit, 0, n * 8);
        unit[0] = 1;
        crypto_montgomery_multiply(out, result, unit, mod, inverse, n);
}

/* Decode the public operation once for both RSA signature encodings.  The
   signature representative is an integer in [0,n), never an arbitrary byte
   string reduced modulo n, and a usable RSA public exponent is odd and at
   least three. */
static bool crypto_rsa_prepare(p8 address_to n_bytes, positive n_length,
                               p64 exponent, p8 address_to sig,
                               positive sig_length, p64 address_to mod,
                               p64 address_to base,
                               positive address_to limbs)
{
        p8 padded[512];

        if (n_length > sizeof(padded) || n_length < 256 ||
            sig_length != n_length || !n_bytes[0] ||
            (n_length == 256 && !(n_bytes[0] & 0x80)) ||
            !(n_bytes[n_length - 1] & 1) || exponent < 3 ||
            !(exponent & 1))
                return false;

        address_to limbs = (n_length + 7) / 8;
        memory_fill(mod, 0, CRYPTO_RSA_LIMBS * sizeof(p64));
        memory_fill(base, 0, CRYPTO_RSA_LIMBS * sizeof(p64));
        memory_fill(padded, 0, sizeof(padded));
        memory_copy(padded + address_to limbs * 8 - n_length, n_bytes,
                    n_length);
        crypto_fe_load_be(mod, padded, address_to limbs);
        memory_fill(padded, 0, sizeof(padded));
        memory_copy(padded + address_to limbs * 8 - sig_length, sig,
                    sig_length);
        crypto_fe_load_be(base, padded, address_to limbs);

        return crypto_fe_cmp(base, mod, address_to limbs) < 0;
}

/* EMSA-PKCS1-v1_5: 00 01 FF..FF 00 DigestInfo hash, at least eight FF
   bytes, the DigestInfo naming the hash exactly. */
static bool crypto_rsa_pkcs1(p8 address_to n_bytes, positive n_length,
                             p64 exponent, p8 address_to sig,
                             positive sig_length,
                             const p8 address_to digestinfo,
                             positive digestinfo_length, p8 address_to hash,
                             positive hash_length)
{
        p64 mod[CRYPTO_RSA_LIMBS];
        p64 base[CRYPTO_RSA_LIMBS];
        p64 out[CRYPTO_RSA_LIMBS];
        p8 em[512];
        positive limbs;
        positive k;
        positive i;

        if (!crypto_rsa_prepare(n_bytes, n_length, exponent, sig,
                                sig_length, mod, base, address_of limbs))
                return false;

        crypto_rsa_modexp(out, base, exponent, mod, limbs);
        k = n_length;
        memory_fill(em, 0, sizeof(em));
        {
                p8 full[512];
                crypto_fe_store_be(full, out, limbs);
                memory_copy(em, full + limbs * 8 - k, k);
        }

        if (em[0] != 0x00 || em[1] != 0x01)
                return false;

        i = 2;
        while (i < k && em[i] == 0xff)
                i++;
        if (i < 10 || i >= k || em[i] != 0x00)
                return false;
        i++;
        if (i + digestinfo_length + hash_length != k)
                return false;
        if (memory_compare(em + i, digestinfo, digestinfo_length))
                return false;
        return memory_compare(em + i + digestinfo_length, hash, hash_length) ==
               0;
}

static bool crypto_rsa_pkcs1_sha256(p8 address_to n_bytes, positive n_length,
                                    p64 exponent, p8 address_to sig,
                                    positive sig_length, p8 address_to hash)
{
        static const p8 digestinfo[19] = {
            0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
            0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

        return crypto_rsa_pkcs1(n_bytes, n_length, exponent, sig, sig_length,
                                digestinfo, sizeof digestinfo, hash, 32);
}

static bool crypto_rsa_pkcs1_sha384(p8 address_to n_bytes, positive n_length,
                                    p64 exponent, p8 address_to sig,
                                    positive sig_length, p8 address_to hash)
{
        static const p8 digestinfo[19] = {
            0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
            0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};

        return crypto_rsa_pkcs1(n_bytes, n_length, exponent, sig, sig_length,
                                digestinfo, sizeof digestinfo, hash, 48);
}

static fn crypto_mgf1_sha256(p8 address_to seed, positive seed_length,
                             p8 address_to into, positive want)
{
        positive offset = 0;
        p32 counter = 0;

        while (offset < want)
        {
                crypto_sha256 hash;
                p8 block[32];
                p8 count[4];
                positive take;

                count[0] = (p8)(counter >> 24);
                count[1] = (p8)(counter >> 16);
                count[2] = (p8)(counter >> 8);
                count[3] = (p8)counter;
                crypto_sha256_open(address_of hash);
                crypto_sha256_write(address_of hash, seed, seed_length);
                crypto_sha256_write(address_of hash, count, 4);
                crypto_sha256_close(address_of hash, block);
                take = want - offset;
                if (take > 32)
                        take = 32;
                memory_copy(into + offset, block, take);
                offset += take;
                counter++;
        }
}

/* TLS 1.3 rsa_pss_rsae_sha256: EMSA-PSS with SHA-256, MGF1-SHA-256, salt 32. */
static bool crypto_rsa_pss_sha256(p8 address_to n_bytes, positive n_length,
                                  p64 exponent, p8 address_to sig,
                                  positive sig_length, p8 address_to message,
                                  positive message_length)
{
        p64 mod[CRYPTO_RSA_LIMBS];
        p64 base[CRYPTO_RSA_LIMBS];
        p64 out[CRYPTO_RSA_LIMBS];
        p8 em[512];
        p8 mask[512];
        p8 mhash[32];
        p8 hcheck[32];
        p8 prefix[8];
        crypto_sha256 hash;
        positive limbs;
        positive k;
        positive mod_bits = 0;
        positive em_bits;
        positive unused;
        positive masked;
        positive at;
        positive i;

        if (!crypto_rsa_prepare(n_bytes, n_length, exponent, sig,
                                sig_length, mod, base, address_of limbs))
                return false;

        for (i = 0; i < n_length; i++)
                if (n_bytes[i])
                {
                        p8 value = n_bytes[i];
                        positive bits = 0;

                        while (value)
                        {
                                bits++;
                                value >>= 1;
                        }
                        mod_bits = (n_length - i - 1) * 8 + bits;
                        break;
                }

        if (mod_bits < 8 * 64)
                return false;

        em_bits = mod_bits - 1;
        k = (em_bits + 7) / 8;
        if (k > n_length || k < 32 + 32 + 2)
                return false;

        crypto_rsa_modexp(out, base, exponent, mod, limbs);
        {
                p8 full[512];

                crypto_fe_store_be(full, out, limbs);
                memory_copy(em, full + limbs * 8 - n_length, n_length);
        }

        if (n_length != k)
        {
                if (n_length < k)
                        return false;
                for (i = 0; i < n_length - k; i++)
                        if (em[i])
                                return false;
                memory_copy(em, em + n_length - k, k);
        }

        unused = 8 * k - em_bits;
        if (unused && (em[0] >> (8 - unused)))
                return false;
        if (em[k - 1] != 0xbc)
                return false;

        masked = k - 32 - 1;
        crypto_mgf1_sha256(em + masked, 32, mask, masked);
        for (i = 0; i < masked; i++)
                em[i] ^= mask[i];
        if (unused)
                em[0] &= (p8)(0xff >> unused);

        at = 0;
        while (at < masked && em[at] == 0)
                at++;
        if (at >= masked || em[at] != 0x01)
                return false;
        at++;
        if (masked - at != 32)
                return false;

        crypto_sha256_of(message, message_length, mhash);
        memory_fill(prefix, 0, sizeof(prefix));
        crypto_sha256_open(address_of hash);
        crypto_sha256_write(address_of hash, prefix, 8);
        crypto_sha256_write(address_of hash, mhash, 32);
        crypto_sha256_write(address_of hash, em + at, 32);
        crypto_sha256_close(address_of hash, hcheck);
        return memory_compare(hcheck, em + masked, 32) == 0;
}

#endif
