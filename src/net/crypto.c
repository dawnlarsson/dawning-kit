/*
        Hashes, AES-GCM, X25519 and the signature checks HTTPS needs.

        wget speaks TLS 1.3 with AES-128-GCM, X25519, P-256 and P-384. The chain for that
        handshake is ECDSA on P-256 and P-384, RSA PKCS#1 and RSA-PSS SHA-256;
        Alpine and GitHub still present RSA leaves. SHA-256 compression is
        sha256_compress in library.c, on the same hardware floor as the rest
        of the binary. AES-GCM, X25519 and ECDSA stay C for now. None of
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

static p64 crypto_rotr64(p64 value, positive bits)
{
        return (value >> bits) | (value << (64 - bits));
}

static const p64 crypto_sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

typedef struct
{
        p32 state[8];
        p64 bits;
        p8 block[64];
        positive used;
} crypto_sha256;

typedef struct
{
        p64 state[8];
        p64 bits_hi;
        p64 bits_lo;
        p8 block[128];
        positive used;
} crypto_sha512;

static fn crypto_sha256_open(crypto_sha256 address_to hash)
{
        hash->state[0] = 0x6a09e667;
        hash->state[1] = 0xbb67ae85;
        hash->state[2] = 0x3c6ef372;
        hash->state[3] = 0xa54ff53a;
        hash->state[4] = 0x510e527f;
        hash->state[5] = 0x9b05688c;
        hash->state[6] = 0x1f83d9ab;
        hash->state[7] = 0x5be0cd19;
        hash->bits = 0;
        hash->used = 0;
}

static fn crypto_sha256_block(crypto_sha256 address_to hash, p8 address_to block)
{
        sha256_compress(hash->state, block);
}

static fn crypto_sha256_write(crypto_sha256 address_to hash, p8 address_to data,
                              positive length)
{
        hash->bits += (p64)length * 8;

        while (length)
        {
                positive take = 64 - hash->used;

                if (take > length)
                        take = length;

                memory_copy(hash->block + hash->used, data, take);
                hash->used += take;
                data += take;
                length -= take;

                if (hash->used == 64)
                {
                        crypto_sha256_block(hash, hash->block);
                        hash->used = 0;
                }
        }
}

static fn crypto_sha256_close(crypto_sha256 address_to hash, p8 address_to out)
{
        positive i;

        hash->block[hash->used++] = 0x80;
        if (hash->used > 56)
        {
                while (hash->used < 64)
                        hash->block[hash->used++] = 0;
                crypto_sha256_block(hash, hash->block);
                hash->used = 0;
        }

        while (hash->used < 56)
                hash->block[hash->used++] = 0;

        crypto_put_be64(hash->block + 56, hash->bits);
        crypto_sha256_block(hash, hash->block);

        for (i = 0; i < 8; i++)
                crypto_put_be32(out + i * 4, hash->state[i]);
}

static fn crypto_sha256_of(p8 address_to data, positive length, p8 address_to out)
{
        crypto_sha256 hash;

        crypto_sha256_open(address_of hash);
        crypto_sha256_write(address_of hash, data, length);
        crypto_sha256_close(address_of hash, out);
}

static fn crypto_sha512_open_iv(crypto_sha512 address_to hash, const p64 address_to iv)
{
        positive i;

        for (i = 0; i < 8; i++)
                hash->state[i] = iv[i];
        hash->bits_hi = 0;
        hash->bits_lo = 0;
        hash->used = 0;
}

static fn crypto_sha384_open(crypto_sha512 address_to hash)
{
        static const p64 iv[8] = {
            0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull,
            0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
            0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
            0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};

        crypto_sha512_open_iv(hash, iv);
}

static fn crypto_sha512_block(crypto_sha512 address_to hash, p8 address_to block)
{
        p64 w[80];
        p64 a, b, c, d, e, f, g, h;
        positive i;

        for (i = 0; i < 16; i++)
                w[i] = crypto_be64(block + i * 8);

        for (i = 16; i < 80; i++)
        {
                p64 s0 = crypto_rotr64(w[i - 15], 1) ^ crypto_rotr64(w[i - 15], 8) ^
                         (w[i - 15] >> 7);
                p64 s1 = crypto_rotr64(w[i - 2], 19) ^ crypto_rotr64(w[i - 2], 61) ^
                         (w[i - 2] >> 6);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        a = hash->state[0];
        b = hash->state[1];
        c = hash->state[2];
        d = hash->state[3];
        e = hash->state[4];
        f = hash->state[5];
        g = hash->state[6];
        h = hash->state[7];

        for (i = 0; i < 80; i++)
        {
                p64 s1 = crypto_rotr64(e, 14) ^ crypto_rotr64(e, 18) ^
                         crypto_rotr64(e, 41);
                p64 ch = (e & f) ^ ((~e) & g);
                p64 t1 = h + s1 + ch + crypto_sha512_k[i] + w[i];
                p64 s0 = crypto_rotr64(a, 28) ^ crypto_rotr64(a, 34) ^
                         crypto_rotr64(a, 39);
                p64 maj = (a & b) ^ (a & c) ^ (b & c);
                p64 t2 = s0 + maj;

                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
        }

        hash->state[0] += a;
        hash->state[1] += b;
        hash->state[2] += c;
        hash->state[3] += d;
        hash->state[4] += e;
        hash->state[5] += f;
        hash->state[6] += g;
        hash->state[7] += h;
}

static fn crypto_sha512_write(crypto_sha512 address_to hash, p8 address_to data,
                              positive length)
{
        p64 add = (p64)length * 8;

        hash->bits_lo += add;
        if (hash->bits_lo < add)
                hash->bits_hi++;

        while (length)
        {
                positive take = 128 - hash->used;

                if (take > length)
                        take = length;

                memory_copy(hash->block + hash->used, data, take);
                hash->used += take;
                data += take;
                length -= take;

                if (hash->used == 128)
                {
                        crypto_sha512_block(hash, hash->block);
                        hash->used = 0;
                }
        }
}

static fn crypto_sha384_close(crypto_sha512 address_to hash, p8 address_to out)
{
        positive i;

        hash->block[hash->used++] = 0x80;
        if (hash->used > 112)
        {
                while (hash->used < 128)
                        hash->block[hash->used++] = 0;
                crypto_sha512_block(hash, hash->block);
                hash->used = 0;
        }

        while (hash->used < 112)
                hash->block[hash->used++] = 0;

        crypto_put_be64(hash->block + 112, hash->bits_hi);
        crypto_put_be64(hash->block + 120, hash->bits_lo);
        crypto_sha512_block(hash, hash->block);

        for (i = 0; i < 6; i++)
                crypto_put_be64(out + i * 8, hash->state[i]);
}

static fn crypto_sha384(p8 address_to data, positive length, p8 address_to out)
{
        crypto_sha512 hash;

        crypto_sha384_open(address_of hash);
        crypto_sha512_write(address_of hash, data, length);
        crypto_sha384_close(address_of hash, out);
}

/* Volatile stores keep secret erasure observable to the compiler. */
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

static p8 crypto_xtime(p8 value)
{
        return (p8)((value << 1) ^
                    (0x1b & (p8)(0 - (value >> 7))));
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

#if X64
typedef p64 crypto_aes_vector
    __attribute__((vector_size(16), aligned(1), may_alias));
typedef long long crypto_aes_vector_signed __attribute__((vector_size(16)));

static p8 crypto_aes_hardware_state;

static bool crypto_aes_hardware(void)
{
        if (!crypto_aes_hardware_state)
        {
                p32 leaf = 1;
                p32 ebx;
                p32 features = 0;
                p32 edx;

                __asm__ volatile("cpuid"
                                 : "+a"(leaf), "=b"(ebx), "+c"(features),
                                   "=d"(edx));
                (void)ebx;
                (void)edx;
                crypto_aes_hardware_state =
                    features & ((p32)1 << 25) ? 2 : 1;
        }

        return crypto_aes_hardware_state == 2;
}

static __attribute__((target("aes,sse2"))) fn
crypto_aes128_encrypt_hardware(p8 address_to round, p8 address_to in,
                               p8 address_to out)
{
        crypto_aes_vector round_key =
            *(crypto_aes_vector address_to)round;
        crypto_aes_vector state =
            *(crypto_aes_vector address_to)in ^ round_key;
        positive round_at;

        for (round_at = 1; round_at < 10; round_at++)
        {
                round_key = *(crypto_aes_vector address_to)
                    (round + round_at * 16);
                state = (crypto_aes_vector)__builtin_ia32_aesenc128(
                    (crypto_aes_vector_signed)state,
                    (crypto_aes_vector_signed)round_key);
        }
        round_key = *(crypto_aes_vector address_to)(round + 160);
        state = (crypto_aes_vector)__builtin_ia32_aesenclast128(
            (crypto_aes_vector_signed)state,
            (crypto_aes_vector_signed)round_key);
        *(crypto_aes_vector address_to)out = state;

        state ^= state;
        round_key ^= round_key;
        __asm__ volatile("" : "+x"(state), "+x"(round_key));
}
#endif

static fn crypto_aes128_encrypt_portable(p8 address_to round, p8 address_to in,
                                         p8 address_to out)
{
        p8 s[16];
        p8 n[16];
        positive round_at;
        positive i;

        memory_copy(s, in, 16);

        for (i = 0; i < 4; i++)
        {
                p32 word = crypto_be32(s + i * 4) ^
                           crypto_be32(round + i * 4);
                crypto_put_be32(s + i * 4, word);
        }

        for (round_at = 1; round_at <= 10; round_at++)
        {
                static const p8 shift[16] = {0, 5, 10, 15, 4, 9, 14, 3,
                                             8, 13, 2, 7, 12, 1, 6, 11};

                for (i = 0; i < 16; i++)
                        n[i] = crypto_aes_substitute(s[shift[i]]);

                if (round_at < 10)
                {
                        for (i = 0; i < 16; i += 4)
                        {
                                p8 a = n[i], b = n[i + 1], c = n[i + 2],
                                   d = n[i + 3];
                                n[i] = crypto_xtime(a) ^ crypto_xtime(b) ^ b ^ c ^
                                       d;
                                n[i + 1] = a ^ crypto_xtime(b) ^ crypto_xtime(c) ^
                                           c ^ d;
                                n[i + 2] = a ^ b ^ crypto_xtime(c) ^ crypto_xtime(d) ^
                                           d;
                                n[i + 3] = crypto_xtime(a) ^ a ^ b ^ c ^
                                           crypto_xtime(d);
                        }
                }

                memory_copy(s, n, 16);
                for (i = 0; i < 4; i++)
                {
                        p32 word = crypto_be32(s + i * 4) ^
                                   crypto_be32(round + round_at * 16 + i * 4);
                        crypto_put_be32(s + i * 4, word);
                }
        }

        memory_copy(out, s, 16);
        crypto_forget(n, sizeof n);
        crypto_forget(s, sizeof s);
}

static fn crypto_aes128_encrypt(p8 address_to round, p8 address_to in,
                                p8 address_to out)
{
#if X64
        if (crypto_aes_hardware())
        {
                crypto_aes128_encrypt_hardware(round, in, out);
                return;
        }
#endif
        crypto_aes128_encrypt_portable(round, in, out);
}

/*
        GHASH over a span, the last partial block zero padded. The multiply
        is ghash_blocks in library.c; GCM's framing stays here.
*/
static fn crypto_ghash_span(p8 address_to state, p8 address_to h,
                            p8 address_to bytes, positive length)
{
        positive whole = length / 16;
        p8 padded[16];

        ghash_blocks(state, h, bytes, whole);

        if (length % 16)
        {
                memory_fill(padded, 0, 16);
                memory_copy(padded, bytes + whole * 16, length % 16);
                ghash_blocks(state, h, padded, 1);
                crypto_forget(padded, sizeof padded);
        }
}

static fn crypto_aesgcm_crypt(p8 address_to key, p8 address_to iv,
                              p8 address_to aad, positive aad_length,
                              p8 address_to text, positive text_length,
                              p8 address_to tag, bool encrypt)
{
        p8 round[176];
        p8 h[16];
        p8 j0[16];
        p8 counter[16];
        p8 s[16];
        p8 zero[16];
        p8 enc[16];
        p8 padded[16];
        positive i;
        positive at;

        crypto_aes128_expand(key, round);
        memory_fill(zero, 0, 16);
        crypto_aes128_encrypt(round, zero, h);

        memory_copy(j0, iv, 12);
        j0[12] = 0;
        j0[13] = 0;
        j0[14] = 0;
        j0[15] = 1;

        memory_copy(counter, j0, 16);
        memory_fill(s, 0, 16);

        crypto_ghash_span(s, h, aad, aad_length);

        //      GHASH reads the ciphertext both ways: before the counter
        //      stream comes off it on the way in, after it goes on on the
        //      way out. So the whole span is hashed in one call.
        if (!encrypt)
                crypto_ghash_span(s, h, text, text_length);

        at = 0;
        while (at < text_length)
        {
                positive take = text_length - at;

                if (take > 16)
                        take = 16;

                {
                        p32 n = crypto_be32(counter + 12) + 1;
                        crypto_put_be32(counter + 12, n);
                }

                crypto_aes128_encrypt(round, counter, enc);

                for (i = 0; i < take; i++)
                        text[at + i] ^= enc[i];

                at += take;
        }

        if (encrypt)
                crypto_ghash_span(s, h, text, text_length);

        memory_fill(padded, 0, 16);
        crypto_put_be64(padded, (p64)aad_length * 8);
        crypto_put_be64(padded + 8, (p64)text_length * 8);
        ghash_blocks(s, h, padded, 1);

        crypto_aes128_encrypt(round, j0, enc);
        for (i = 0; i < 16; i++)
                tag[i] = s[i] ^ enc[i];

        crypto_forget(round, sizeof round);
        crypto_forget(h, sizeof h);
        crypto_forget(j0, sizeof j0);
        crypto_forget(counter, sizeof counter);
        crypto_forget(s, sizeof s);
        crypto_forget(zero, sizeof zero);
        crypto_forget(enc, sizeof enc);
        crypto_forget(padded, sizeof padded);
}

static fn crypto_aesgcm_encrypt(p8 address_to key, p8 address_to iv,
                                p8 address_to aad, positive aad_length,
                                p8 address_to text, positive text_length,
                                p8 address_to tag)
{
        crypto_aesgcm_crypt(key, iv, aad, aad_length, text, text_length, tag,
                            true);
}

static bool crypto_aesgcm_decrypt(p8 address_to key, p8 address_to iv,
                                  p8 address_to aad, positive aad_length,
                                  p8 address_to text, positive text_length,
                                  p8 address_to tag)
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
   every limb and is suitable for decisions derived from private points. */
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

static fn crypto_fe_add(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const p64 address_to p, positive n)
{
        p64 sum[CRYPTO_FE_MAX];
        p64 reduced[CRYPTO_FE_MAX];
        crypto_wide carry = 0;
        p64 borrow;
        p64 reduce;

        for (positive i = 0; i < n; i++)
        {
                carry += (crypto_wide)a[i] + b[i];
                sum[i] = (p64)carry;
                carry >>= 64;
        }

        borrow = crypto_fe_subtract_raw(reduced, sum, p, n);
        reduce = (p64)carry | (borrow ^ 1);
        crypto_fe_select(d, sum, reduced, n, reduce);
        crypto_forget(sum, sizeof sum);
        crypto_forget(reduced, sizeof reduced);
        crypto_forget(address_of carry, sizeof carry);
        crypto_forget(address_of borrow, sizeof borrow);
        crypto_forget(address_of reduce, sizeof reduce);
}

static fn crypto_fe_sub(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const p64 address_to p, positive n)
{
        p64 difference[CRYPTO_FE_MAX];
        p64 restored[CRYPTO_FE_MAX];
        p64 borrow = crypto_fe_subtract_raw(difference, a, b, n);
        crypto_wide carry = 0;

        for (positive i = 0; i < n; i++)
        {
                carry += (crypto_wide)difference[i] + p[i];
                restored[i] = (p64)carry;
                carry >>= 64;
        }

        crypto_fe_select(d, difference, restored, n, borrow);
        crypto_forget(difference, sizeof difference);
        crypto_forget(restored, sizeof restored);
        crypto_forget(address_of borrow, sizeof borrow);
        crypto_forget(address_of carry, sizeof carry);
}

static fn crypto_fe_mul(p64 address_to d, const p64 address_to a,
                        const p64 address_to b, const p64 address_to p, positive n)
{
        p64 product[CRYPTO_FE_MAX * 2];
        p64 remainder[CRYPTO_FE_MAX + 1];
        p64 reduced[CRYPTO_FE_MAX + 1];
        positive i;
        positive j;
        positive bit;

        memory_fill(product, 0, sizeof product);
        for (i = 0; i < n; i++)
        {
                crypto_wide carry = 0;
                for (j = 0; j < n; j++)
                {
                        carry += (crypto_wide)product[i + j] +
                                 (crypto_wide)a[i] * b[j];
                        product[i + j] = (p64)carry;
                        carry >>= 64;
                }
                product[i + n] = (p64)carry;
        }

        memory_fill(remainder, 0, sizeof remainder);
        bit = n * 2 * 64;
        while (bit)
        {
                crypto_wide carry = 0;
                p64 borrow;
                p64 reduce;
                p64 mask;

                bit--;
                for (i = 0; i <= n; i++)
                {
                        crypto_wide u =
                            ((crypto_wide)remainder[i] << 1) | carry;
                        remainder[i] = (p64)u;
                        carry = u >> 64;
                }
                remainder[0] |=
                    (product[bit / 64] >> (bit % 64)) & 1;

                borrow = crypto_fe_subtract_raw(
                    reduced, remainder, p, n);
                reduced[n] = remainder[n] - borrow;
                reduce = remainder[n] | (borrow ^ 1);
                mask = 0 - reduce;
                for (i = 0; i <= n; i++)
                        remainder[i] = (remainder[i] & ~mask) |
                                       (reduced[i] & mask);
        }

        memory_copy(d, remainder, n * 8);
        crypto_forget(product, sizeof product);
        crypto_forget(remainder, sizeof remainder);
        crypto_forget(reduced, sizeof reduced);
}

static fn crypto_fe_sqr(p64 address_to d, const p64 address_to a,
                        const p64 address_to p, positive n)
{
        crypto_fe_mul(d, a, a, p, n);
}

static fn crypto_fe_inv(p64 address_to d, const p64 address_to a,
                        const p64 address_to p, positive n)
{
        /* Fermat: a^(p-2) */
        p64 exp[CRYPTO_FE_MAX];
        p64 base[CRYPTO_FE_MAX];
        p64 result[CRYPTO_FE_MAX];
        p64 two[CRYPTO_FE_MAX];
        p64 tmp[CRYPTO_FE_MAX];
        positive i;
        positive bit;
        positive bits = n * 64;

        memory_fill(two, 0, sizeof(two));
        two[0] = 2;
        crypto_fe_sub(exp, p, two, p, n);
        memory_copy(base, a, n * 8);
        memory_fill(result, 0, sizeof(result));
        result[0] = 1;

        for (i = 0; i < bits; i++)
        {
                if ((exp[i / 64] >> (i % 64)) & 1)
                {
                        crypto_fe_mul(tmp, result, base, p, n);
                        memory_copy(result, tmp, n * 8);
                }
                {
                        crypto_fe_sqr(tmp, base, p, n);
                        memory_copy(base, tmp, n * 8);
                }
        }

        memory_copy(d, result, n * 8);
        crypto_forget(exp, sizeof exp);
        crypto_forget(base, sizeof base);
        crypto_forget(result, sizeof result);
        crypto_forget(two, sizeof two);
        crypto_forget(tmp, sizeof tmp);
        (void)bit;
}

typedef struct
{
        p64 x[CRYPTO_FE_MAX];
        p64 y[CRYPTO_FE_MAX];
        p64 z[CRYPTO_FE_MAX];
        positive n;
        const p64 address_to p;
} crypto_point;

static fn crypto_point_zero(crypto_point address_to q, const p64 address_to p,
                              positive n)
{
        memory_fill(q, 0, sizeof(*q));
        q->n = n;
        q->p = p;
        q->z[0] = 0;
}

static fn crypto_point_set_xy(crypto_point address_to q, const p64 address_to x,
                              const p64 address_to y, const p64 address_to p,
                              positive n)
{
        memory_fill(q, 0, sizeof(*q));
        q->n = n;
        q->p = p;
        memory_copy(q->x, x, n * 8);
        memory_copy(q->y, y, n * 8);
        q->z[0] = 1;
}

static fn crypto_point_double(crypto_point address_to r, crypto_point address_to p)
{
        /* a = -3 */
        p64 xx[CRYPTO_FE_MAX], zz[CRYPTO_FE_MAX], yy[CRYPTO_FE_MAX];
        p64 yyyy[CRYPTO_FE_MAX], s[CRYPTO_FE_MAX], m[CRYPTO_FE_MAX];
        p64 tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        positive n = p->n;
        const p64 address_to mod = p->p;

        if (crypto_fe_is_zero(p->z, n))
        {
                *r = *p;
                return;
        }

        crypto_fe_sqr(xx, p->x, mod, n);
        crypto_fe_sqr(zz, p->z, mod, n);
        crypto_fe_sqr(zz, zz, mod, n);
        crypto_fe_sqr(yy, p->y, mod, n);
        crypto_fe_sqr(yyyy, yy, mod, n);

        crypto_fe_add(tmp, p->x, yy, mod, n);
        crypto_fe_sqr(tmp2, tmp, mod, n);
        crypto_fe_sub(tmp, tmp2, xx, mod, n);
        crypto_fe_sub(tmp, tmp, yyyy, mod, n);
        crypto_fe_add(s, tmp, tmp, mod, n);

        crypto_fe_sub(tmp, xx, zz, mod, n);
        crypto_fe_add(m, tmp, tmp, mod, n);
        crypto_fe_add(m, m, tmp, mod, n);

        crypto_fe_sqr(tmp, m, mod, n);
        crypto_fe_add(tmp2, s, s, mod, n);
        crypto_fe_sub(r->x, tmp, tmp2, mod, n);

        crypto_fe_sub(tmp, s, r->x, mod, n);
        crypto_fe_mul(tmp2, m, tmp, mod, n);
        crypto_fe_add(tmp, yyyy, yyyy, mod, n);
        crypto_fe_add(tmp, tmp, tmp, mod, n);
        crypto_fe_add(tmp, tmp, tmp, mod, n);
        crypto_fe_sub(r->y, tmp2, tmp, mod, n);

        crypto_fe_add(tmp, p->y, p->y, mod, n);
        crypto_fe_mul(r->z, tmp, p->z, mod, n);
        r->n = n;
        r->p = mod;
}

static fn crypto_point_add(crypto_point address_to r, crypto_point address_to p,
                           crypto_point address_to q)
{
        p64 z1z1[CRYPTO_FE_MAX], z2z2[CRYPTO_FE_MAX];
        p64 u1[CRYPTO_FE_MAX], u2[CRYPTO_FE_MAX], s1[CRYPTO_FE_MAX], s2[CRYPTO_FE_MAX];
        p64 h[CRYPTO_FE_MAX], rr[CRYPTO_FE_MAX], hh[CRYPTO_FE_MAX], hhh[CRYPTO_FE_MAX];
        p64 v[CRYPTO_FE_MAX], tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        positive n = p->n;
        const p64 address_to mod = p->p;

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

        crypto_fe_sqr(z1z1, p->z, mod, n);
        crypto_fe_sqr(z2z2, q->z, mod, n);
        crypto_fe_mul(u1, p->x, z2z2, mod, n);
        crypto_fe_mul(u2, q->x, z1z1, mod, n);
        crypto_fe_mul(tmp, q->z, z2z2, mod, n);
        crypto_fe_mul(s1, p->y, tmp, mod, n);
        crypto_fe_mul(tmp, p->z, z1z1, mod, n);
        crypto_fe_mul(s2, q->y, tmp, mod, n);

        crypto_fe_sub(h, u2, u1, mod, n);
        crypto_fe_sub(rr, s2, s1, mod, n);

        if (crypto_fe_is_zero(h, n))
        {
                if (crypto_fe_is_zero(rr, n))
                {
                        crypto_point_double(r, p);
                        return;
                }
                crypto_point_zero(r, mod, n);
                return;
        }

        crypto_fe_sqr(hh, h, mod, n);
        crypto_fe_mul(hhh, h, hh, mod, n);
        crypto_fe_mul(v, u1, hh, mod, n);

        crypto_fe_sqr(tmp, rr, mod, n);
        crypto_fe_sub(tmp, tmp, hhh, mod, n);
        crypto_fe_add(tmp2, v, v, mod, n);
        crypto_fe_sub(r->x, tmp, tmp2, mod, n);

        crypto_fe_sub(tmp, v, r->x, mod, n);
        crypto_fe_mul(tmp2, rr, tmp, mod, n);
        crypto_fe_mul(tmp, s1, hhh, mod, n);
        crypto_fe_sub(r->y, tmp2, tmp, mod, n);

        crypto_fe_mul(tmp, p->z, q->z, mod, n);
        crypto_fe_mul(r->z, tmp, h, mod, n);
        r->n = n;
        r->p = mod;
}

/* The ECDH multiplier cannot use the public-signature helpers above: their
   exceptional-point branches and bit-conditional addition reveal a private
   scalar to a branch or cache observer.  These helpers keep the ladder's
   adjacent-point invariant and select its infinity cases with masks.  Field
   reduction is likewise branchless, so each scalar bit follows the same
   operations and addresses. */
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
        d->p = a->p;
        crypto_forget(address_of mask, sizeof mask);
}

static fn crypto_point_cswap(crypto_point address_to a,
                             crypto_point address_to b, p64 swap)
{
        p64 mask = 0 - swap;

        for (positive i = 0; i < CRYPTO_FE_MAX; i++)
        {
                p64 x = mask & (a->x[i] ^ b->x[i]);
                p64 y = mask & (a->y[i] ^ b->y[i]);
                p64 z = mask & (a->z[i] ^ b->z[i]);

                a->x[i] ^= x;
                b->x[i] ^= x;
                a->y[i] ^= y;
                b->y[i] ^= y;
                a->z[i] ^= z;
                b->z[i] ^= z;
        }
        crypto_forget(address_of mask, sizeof mask);
}

static fn crypto_point_double_private(crypto_point address_to r,
                                      const crypto_point address_to p)
{
        p64 xx[CRYPTO_FE_MAX], zz[CRYPTO_FE_MAX], yy[CRYPTO_FE_MAX];
        p64 yyyy[CRYPTO_FE_MAX], s[CRYPTO_FE_MAX], m[CRYPTO_FE_MAX];
        p64 tmp[CRYPTO_FE_MAX], tmp2[CRYPTO_FE_MAX];
        positive n = p->n;
        const p64 address_to mod = p->p;
        crypto_point result;

        crypto_point_zero(address_of result, mod, n);
        crypto_fe_sqr(xx, p->x, mod, n);
        crypto_fe_sqr(zz, p->z, mod, n);
        crypto_fe_sqr(zz, zz, mod, n);
        crypto_fe_sqr(yy, p->y, mod, n);
        crypto_fe_sqr(yyyy, yy, mod, n);

        crypto_fe_add(tmp, p->x, yy, mod, n);
        crypto_fe_sqr(tmp2, tmp, mod, n);
        crypto_fe_sub(tmp, tmp2, xx, mod, n);
        crypto_fe_sub(tmp, tmp, yyyy, mod, n);
        crypto_fe_add(s, tmp, tmp, mod, n);

        crypto_fe_sub(tmp, xx, zz, mod, n);
        crypto_fe_add(m, tmp, tmp, mod, n);
        crypto_fe_add(m, m, tmp, mod, n);

        crypto_fe_sqr(tmp, m, mod, n);
        crypto_fe_add(tmp2, s, s, mod, n);
        crypto_fe_sub(result.x, tmp, tmp2, mod, n);

        crypto_fe_sub(tmp, s, result.x, mod, n);
        crypto_fe_mul(tmp2, m, tmp, mod, n);
        crypto_fe_add(tmp, yyyy, yyyy, mod, n);
        crypto_fe_add(tmp, tmp, tmp, mod, n);
        crypto_fe_add(tmp, tmp, tmp, mod, n);
        crypto_fe_sub(result.y, tmp2, tmp, mod, n);

        crypto_fe_add(tmp, p->y, p->y, mod, n);
        crypto_fe_mul(result.z, tmp, p->z, mod, n);
        *r = result;

        crypto_forget(xx, sizeof xx);
        crypto_forget(zz, sizeof zz);
        crypto_forget(yy, sizeof yy);
        crypto_forget(yyyy, sizeof yyyy);
        crypto_forget(s, sizeof s);
        crypto_forget(m, sizeof m);
        crypto_forget(tmp, sizeof tmp);
        crypto_forget(tmp2, sizeof tmp2);
        crypto_forget(address_of result, sizeof result);
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
        const p64 address_to mod = p->p;
        crypto_point sum;
        p64 p_infinity;
        p64 q_infinity;

        crypto_point_zero(address_of sum, mod, n);
        crypto_fe_sqr(z1z1, p->z, mod, n);
        crypto_fe_sqr(z2z2, q->z, mod, n);
        crypto_fe_mul(u1, p->x, z2z2, mod, n);
        crypto_fe_mul(u2, q->x, z1z1, mod, n);
        crypto_fe_mul(tmp, q->z, z2z2, mod, n);
        crypto_fe_mul(s1, p->y, tmp, mod, n);
        crypto_fe_mul(tmp, p->z, z1z1, mod, n);
        crypto_fe_mul(s2, q->y, tmp, mod, n);

        crypto_fe_sub(h, u2, u1, mod, n);
        crypto_fe_sub(rr, s2, s1, mod, n);
        crypto_fe_sqr(hh, h, mod, n);
        crypto_fe_mul(hhh, h, hh, mod, n);
        crypto_fe_mul(v, u1, hh, mod, n);

        crypto_fe_sqr(tmp, rr, mod, n);
        crypto_fe_sub(tmp, tmp, hhh, mod, n);
        crypto_fe_add(tmp2, v, v, mod, n);
        crypto_fe_sub(sum.x, tmp, tmp2, mod, n);

        crypto_fe_sub(tmp, v, sum.x, mod, n);
        crypto_fe_mul(tmp2, rr, tmp, mod, n);
        crypto_fe_mul(tmp, s1, hhh, mod, n);
        crypto_fe_sub(sum.y, tmp2, tmp, mod, n);

        crypto_fe_mul(tmp, p->z, q->z, mod, n);
        crypto_fe_mul(sum.z, tmp, h, mod, n);

        /* Ladder operands differ by the fixed non-infinite input point.  The
           generic formula therefore cannot see the equal-point exception;
           opposite points already produce z=0.  Only the two fixed startup
           infinity cases need masked selection. */
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

static fn crypto_point_scalar_private(
    crypto_point address_to r, const crypto_point address_to p,
    const p64 address_to k, crypto_scalar_schedule address_to schedule)
{
        crypto_point left;
        crypto_point right = *p;
        crypto_point sum;
        crypto_point doubled;
        positive bits = p->n * 64;
        p64 bit = 0;

        crypto_point_zero(address_of left, p->p, p->n);
        if (schedule)
                memory_fill(schedule, 0, sizeof(*schedule));

        for (positive at = bits; at; at--)
        {
                bit = (k[(at - 1) / 64] >> ((at - 1) % 64)) & 1;
                crypto_point_cswap(address_of left, address_of right, bit);
                crypto_point_add_private(address_of sum, address_of left,
                                         address_of right);
                crypto_point_double_private(address_of doubled,
                                             address_of left);
                left = doubled;
                right = sum;
                crypto_point_cswap(address_of left, address_of right, bit);

                if (schedule)
                {
                        schedule->bits++;
                        schedule->point_adds++;
                        schedule->point_doubles++;
                        schedule->conditional_swaps += 2;
                        schedule->conditional_selects += 2;
                }
        }

        *r = left;
        crypto_forget(address_of left, sizeof left);
        crypto_forget(address_of right, sizeof right);
        crypto_forget(address_of sum, sizeof sum);
        crypto_forget(address_of doubled, sizeof doubled);
        crypto_forget(address_of bit, sizeof bit);
}

static fn crypto_point_scalar(crypto_point address_to r, crypto_point address_to p,
                              p64 address_to k)
{
        crypto_point n;
        positive bits = p->n * 64;
        positive i;

        crypto_point_zero(r, p->p, p->n);
        n = *p;

        for (i = 0; i < bits; i++)
        {
                if ((k[i / 64] >> (i % 64)) & 1)
                {
                        crypto_point t;
                        crypto_point_add(address_of t, r, address_of n);
                        *r = t;
                }
                {
                        crypto_point t;
                        crypto_point_double(address_of t, address_of n);
                        n = t;
                }
        }
}

static fn crypto_point_affine(crypto_point address_to p)
{
        p64 zinv[CRYPTO_FE_MAX], z2[CRYPTO_FE_MAX], z3[CRYPTO_FE_MAX];

        if (crypto_fe_is_zero(p->z, p->n))
                goto done;

        crypto_fe_inv(zinv, p->z, p->p, p->n);
        crypto_fe_sqr(z2, zinv, p->p, p->n);
        crypto_fe_mul(z3, z2, zinv, p->p, p->n);
        crypto_fe_mul(p->x, p->x, z2, p->p, p->n);
        crypto_fe_mul(p->y, p->y, z3, p->p, p->n);
        memory_fill(p->z, 0, p->n * 8);
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
                                     positive limbs, const p64 address_to p,
                                     const p8 address_to b_bytes)
{
        p64 x[CRYPTO_FE_MAX], y[CRYPTO_FE_MAX], b[CRYPTO_FE_MAX];
        p64 left[CRYPTO_FE_MAX], right[CRYPTO_FE_MAX];
        p64 x2[CRYPTO_FE_MAX];

        crypto_fe_load_be(x, x_bytes, limbs);
        crypto_fe_load_be(y, y_bytes, limbs);
        crypto_fe_load_be(b, b_bytes, limbs);
        if (crypto_fe_cmp(x, p, limbs) >= 0 ||
            crypto_fe_cmp(y, p, limbs) >= 0)
                return false;

        /* NIST P-256 and P-384 both use y^2 = x^3 - 3x + b.  Affine
           infinity has no encoding, and (0,0) fails this equation. */
        crypto_fe_sqr(left, y, p, limbs);
        crypto_fe_sqr(x2, x, p, limbs);
        crypto_fe_mul(right, x2, x, p, limbs);
        crypto_fe_sub(right, right, x, p, limbs);
        crypto_fe_sub(right, right, x, p, limbs);
        crypto_fe_sub(right, right, x, p, limbs);
        crypto_fe_add(right, right, b, p, limbs);

        return crypto_fe_cmp(left, right, limbs) == 0;
}

static bool crypto_ecdsa_verify(p8 address_to hash, positive hash_length,
                                p8 address_to r_bytes, positive r_length,
                                p8 address_to s_bytes, positive s_length,
                                p8 address_to qx, p8 address_to qy, positive limbs,
                                const p64 address_to p, const p64 address_to n,
                                const p8 address_to gx, const p8 address_to gy,
                                const p8 address_to b)
{
        p64 r[CRYPTO_FE_MAX], s[CRYPTO_FE_MAX], e[CRYPTO_FE_MAX];
        p64 w[CRYPTO_FE_MAX], u1[CRYPTO_FE_MAX], u2[CRYPTO_FE_MAX];
        p64 gx_f[CRYPTO_FE_MAX], gy_f[CRYPTO_FE_MAX], qx_f[CRYPTO_FE_MAX],
            qy_f[CRYPTO_FE_MAX];
        crypto_point g, q, p1, p2, rpoint;
        p8 ehash[48];

        if (!crypto_scalar_from_int_be(r, r_bytes, r_length, n, limbs) ||
            !crypto_scalar_from_int_be(s, s_bytes, s_length, n, limbs) ||
            !crypto_point_is_on_curve(qx, qy, limbs, p, b))
                return false;

        memory_fill(ehash, 0, sizeof(ehash));
        if (hash_length >= limbs * 8)
                memory_copy(ehash, hash, limbs * 8);
        else
                memory_copy(ehash + limbs * 8 - hash_length, hash, hash_length);
        crypto_fe_load_be(e, ehash, limbs);
        while (crypto_fe_cmp(e, n, limbs) >= 0)
                crypto_fe_sub(e, e, n, n, limbs);

        crypto_fe_inv(w, s, n, limbs);
        crypto_fe_mul(u1, e, w, n, limbs);
        crypto_fe_mul(u2, r, w, n, limbs);

        crypto_fe_load_be(gx_f, gx, limbs);
        crypto_fe_load_be(gy_f, gy, limbs);
        crypto_fe_load_be(qx_f, qx, limbs);
        crypto_fe_load_be(qy_f, qy, limbs);

        crypto_point_set_xy(address_of g, gx_f, gy_f, p, limbs);
        crypto_point_set_xy(address_of q, qx_f, qy_f, p, limbs);
        crypto_point_scalar(address_of p1, address_of g, u1);
        crypto_point_scalar(address_of p2, address_of q, u2);
        crypto_point_add(address_of rpoint, address_of p1, address_of p2);
        if (crypto_fe_is_zero(rpoint.z, limbs))
                return false;
        crypto_point_affine(address_of rpoint);
        while (crypto_fe_cmp(rpoint.x, n, limbs) >= 0)
                crypto_fe_sub(rpoint.x, rpoint.x, n, n, limbs);

        return crypto_fe_cmp(rpoint.x, r, limbs) == 0;
}

static bool crypto_ecdsa_p256(p8 address_to hash, positive hash_length,
                              p8 address_to r, positive r_length, p8 address_to s,
                              positive s_length, p8 address_to qx, p8 address_to qy)
{
        return crypto_ecdsa_verify(hash, hash_length, r, r_length, s, s_length,
                                   qx, qy, 4, crypto_p256_p, crypto_p256_n,
                                   crypto_p256_gx_be, crypto_p256_gy_be,
                                   crypto_p256_b_be);
}

static bool crypto_ecdsa_p384(p8 address_to hash, positive hash_length,
                              p8 address_to r, positive r_length, p8 address_to s,
                              positive s_length, p8 address_to qx, p8 address_to qy)
{
        return crypto_ecdsa_verify(hash, hash_length, r, r_length, s, s_length,
                                   qx, qy, 6, crypto_p384_p, crypto_p384_n,
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
                               positive limbs, const p64 address_to p,
                               const p64 address_to n, const p8 address_to gx,
                               const p8 address_to gy)
{
        p64 k[CRYPTO_FE_MAX];
        p64 gx_f[CRYPTO_FE_MAX];
        p64 gy_f[CRYPTO_FE_MAX];
        crypto_point g;
        crypto_point r;
        bool ok = false;

        if (!crypto_scalar_from_int_be(k, scalar, limbs * 8, n, limbs))
                goto done;

        crypto_fe_load_be(gx_f, gx, limbs);
        crypto_fe_load_be(gy_f, gy, limbs);
        crypto_point_set_xy(address_of g, gx_f, gy_f, p, limbs);
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
                               positive limbs, const p64 address_to p,
                               const p64 address_to n, const p8 address_to b)
{
        p64 k[CRYPTO_FE_MAX];
        p64 qx[CRYPTO_FE_MAX];
        p64 qy[CRYPTO_FE_MAX];
        crypto_point q;
        crypto_point r;
        positive coord = limbs * 8;
        bool ok = false;

        if (peer_length != 1 + 2 * coord || peer[0] != 4 ||
            !crypto_point_is_on_curve(peer + 1, peer + 1 + coord, limbs, p,
                                      b) ||
            !crypto_scalar_from_int_be(k, scalar, coord, n, limbs))
                goto done;

        crypto_fe_load_be(qx, peer + 1, limbs);
        crypto_fe_load_be(qy, peer + 1 + coord, limbs);
        crypto_point_set_xy(address_of q, qx, qy, p, limbs);
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
        return crypto_ecdh_public(out, scalar, 4, crypto_p256_p, crypto_p256_n,
                                  crypto_p256_gx_be, crypto_p256_gy_be);
}

static bool crypto_ecdh_p256_shared(p8 address_to out, p8 address_to scalar,
                                    p8 address_to peer)
{
        return crypto_ecdh_shared(out, scalar, peer, 65, 4, crypto_p256_p,
                                  crypto_p256_n, crypto_p256_b_be);
}

static bool crypto_ecdh_p384_public(p8 address_to out, p8 address_to scalar)
{
        return crypto_ecdh_public(out, scalar, 6, crypto_p384_p, crypto_p384_n,
                                  crypto_p384_gx_be, crypto_p384_gy_be);
}

static bool crypto_ecdh_p384_shared(p8 address_to out, p8 address_to scalar,
                                    p8 address_to peer)
{
        return crypto_ecdh_shared(out, scalar, peer, 97, 6, crypto_p384_p,
                                  crypto_p384_n, crypto_p384_b_be);
}

#define CRYPTO_RSA_LIMBS 64

/* RSA operands run to CRYPTO_RSA_LIMBS, ten times what crypto_fe_mul's
   curve-sized scratch holds; routing a 2048- or 4096-bit modulus through it
   wrote the product over the caller's return address.  The public operation
   has no secret, so reduction may branch. */
static fn crypto_rsa_mul(p64 address_to d, p64 address_to a, p64 address_to b,
                         p64 address_to m, positive n)
{
        p64 product[CRYPTO_RSA_LIMBS * 2];
        p64 remainder[CRYPTO_RSA_LIMBS + 1];
        positive i;
        positive j;
        positive bit;

        if (n > CRYPTO_RSA_LIMBS)
        {
                memory_fill(d, 0, n * 8);
                return;
        }

        memory_fill(product, 0, n * 2 * 8);
        for (i = 0; i < n; i++)
        {
                crypto_wide carry = 0;
                for (j = 0; j < n; j++)
                {
                        carry += (crypto_wide)product[i + j] +
                                 (crypto_wide)a[i] * b[j];
                        product[i + j] = (p64)carry;
                        carry >>= 64;
                }
                product[i + n] = (p64)carry;
        }

        memory_fill(remainder, 0, (n + 1) * 8);
        bit = n * 2 * 64;
        while (bit)
        {
                crypto_wide carry = 0;

                bit--;
                for (i = 0; i <= n; i++)
                {
                        crypto_wide u =
                            ((crypto_wide)remainder[i] << 1) | carry;
                        remainder[i] = (p64)u;
                        carry = u >> 64;
                }
                remainder[0] |= (product[bit / 64] >> (bit % 64)) & 1;
                if (remainder[n] || crypto_fe_cmp(remainder, m, n) >= 0)
                        remainder[n] -= crypto_fe_subtract_raw(remainder,
                                                               remainder, m, n);
        }

        memory_copy(d, remainder, n * 8);
}

static fn crypto_rsa_modexp(p64 address_to out, p64 address_to base, p64 exp,
                            p64 address_to mod, positive n)
{
        p64 result[CRYPTO_RSA_LIMBS];
        p64 b[CRYPTO_RSA_LIMBS];
        p64 e = exp;
        positive bits;
        positive i;

        memory_fill(result, 0, n * 8);
        result[0] = 1;
        memory_copy(b, base, n * 8);

        bits = 64;
        for (i = 0; i < bits; i++)
        {
                if ((e >> i) & 1)
                {
                        p64 tmp[CRYPTO_RSA_LIMBS];
                        crypto_rsa_mul(tmp, result, b, mod, n);
                        memory_copy(result, tmp, n * 8);
                }
                {
                        p64 tmp[CRYPTO_RSA_LIMBS];
                        crypto_rsa_mul(tmp, b, b, mod, n);
                        memory_copy(b, tmp, n * 8);
                }
        }

        memory_copy(out, result, n * 8);
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

static bool crypto_rsa_pkcs1_sha256(p8 address_to n_bytes, positive n_length,
                                    p64 exponent, p8 address_to sig,
                                    positive sig_length, p8 address_to hash)
{
        static const p8 digestinfo[19] = {
            0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
            0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
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
        if (i + 19 + 32 != k)
                return false;
        if (memory_compare(em + i, digestinfo, 19))
                return false;
        return memory_compare(em + i + 19, hash, 32) == 0;
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
