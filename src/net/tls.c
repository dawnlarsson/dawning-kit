/*
        TLS 1.3 client for wget.

        One cipher: TLS_AES_128_GCM_SHA256. One group: X25519. Certificates
        walk to ISRG Root X2, ISRG Root X1, or USERTrust ECC -- the three
        roots the bowl bootstrap hosts actually present (Arch on X2, Alpine
        and GitHub user content on X1, github.com on USERTrust/Sectigo).
        Signature algorithms advertised are ecdsa_secp256r1_sha256,
        ecdsa_secp384r1_sha384 and rsa_pss_rsae_sha256. close_notify is a
        clean end of the body, not a handshake failure.
        --no-check-certificate skips the chain and still encrypts.
*/

#ifndef STANDARD_MODERN_C_NET_TLS
#define STANDARD_MODERN_C_NET_TLS

#include "crypto.c"
#include "wait.c"

#define TLS_RECORD_MAX 16640
#define TLS_HS_MAX 16384
#define TLS_HANDSHAKE_SECONDS 30
#define TLS_OK 0
#define TLS_FAIL (-1)
#define TLS_EOF 1

#define TLS_CT_CCS 20
#define TLS_CT_ALERT 21
#define TLS_CT_HANDSHAKE 22
#define TLS_CT_APP 23

#define TLS_HS_CLIENT_HELLO 1
#define TLS_HS_SERVER_HELLO 2
#define TLS_HS_NEW_SESSION_TICKET 4
#define TLS_HS_ENCRYPTED_EXTS 8
#define TLS_HS_CERTIFICATE 11
#define TLS_HS_CERT_VERIFY 15
#define TLS_HS_FINISHED 20

static const p8 tls_isrg_x2_x[48] = {
    0xcd, 0x9b, 0xd5, 0x9f, 0x80, 0x83, 0x0a, 0xec, 0x09, 0x4a, 0xf3, 0x16,
    0x4a, 0x3e, 0x5c, 0xcf, 0x77, 0xac, 0xde, 0x67, 0x05, 0x0d, 0x1d, 0x07,
    0xb6, 0xdc, 0x16, 0xfb, 0x5a, 0x8b, 0x14, 0xdb, 0xe2, 0x71, 0x60, 0xc4,
    0xba, 0x45, 0x95, 0x11, 0x89, 0x8e, 0xea, 0x06, 0xdf, 0xf7, 0x2a, 0x16};
static const p8 tls_isrg_x2_y[48] = {
    0x1c, 0xa4, 0xb9, 0xc5, 0xc5, 0x32, 0xe0, 0x03, 0xe0, 0x1e, 0x82, 0x18,
    0x38, 0x8b, 0xd7, 0x45, 0xd8, 0x0a, 0x6a, 0x6e, 0xe6, 0x00, 0x77, 0xfb,
    0x02, 0x51, 0x7d, 0x22, 0xd8, 0x0a, 0x6e, 0x9a, 0x5b, 0x77, 0xdf, 0xf0,
    0xfa, 0x41, 0xec, 0x39, 0xdc, 0x75, 0xca, 0x68, 0x07, 0x0c, 0x1f, 0xea};

static const p8 tls_isrg_x1_n[512] = {
    0xad, 0xe8, 0x24, 0x73, 0xf4, 0x14, 0x37, 0xf3, 0x9b, 0x9e, 0x2b, 0x57,
    0x28, 0x1c, 0x87, 0xbe, 0xdc, 0xb7, 0xdf, 0x38, 0x90, 0x8c, 0x6e, 0x3c,
    0xe6, 0x57, 0xa0, 0x78, 0xf7, 0x75, 0xc2, 0xa2, 0xfe, 0xf5, 0x6a, 0x6e,
    0xf6, 0x00, 0x4f, 0x28, 0xdb, 0xde, 0x68, 0x86, 0x6c, 0x44, 0x93, 0xb6,
    0xb1, 0x63, 0xfd, 0x14, 0x12, 0x6b, 0xbf, 0x1f, 0xd2, 0xea, 0x31, 0x9b,
    0x21, 0x7e, 0xd1, 0x33, 0x3c, 0xba, 0x48, 0xf5, 0xdd, 0x79, 0xdf, 0xb3,
    0xb8, 0xff, 0x12, 0xf1, 0x21, 0x9a, 0x4b, 0xc1, 0x8a, 0x86, 0x71, 0x69,
    0x4a, 0x66, 0x66, 0x6c, 0x8f, 0x7e, 0x3c, 0x70, 0xbf, 0xad, 0x29, 0x22,
    0x06, 0xf3, 0xe4, 0xc0, 0xe6, 0x80, 0xae, 0xe2, 0x4b, 0x8f, 0xb7, 0x99,
    0x7e, 0x94, 0x03, 0x9f, 0xd3, 0x47, 0x97, 0x7c, 0x99, 0x48, 0x23, 0x53,
    0xe8, 0x38, 0xae, 0x4f, 0x0a, 0x6f, 0x83, 0x2e, 0xd1, 0x49, 0x57, 0x8c,
    0x80, 0x74, 0xb6, 0xda, 0x2f, 0xd0, 0x38, 0x8d, 0x7b, 0x03, 0x70, 0x21,
    0x1b, 0x75, 0xf2, 0x30, 0x3c, 0xfa, 0x8f, 0xae, 0xdd, 0xda, 0x63, 0xab,
    0xeb, 0x16, 0x4f, 0xc2, 0x8e, 0x11, 0x4b, 0x7e, 0xcf, 0x0b, 0xe8, 0xff,
    0xb5, 0x77, 0x2e, 0xf4, 0xb2, 0x7b, 0x4a, 0xe0, 0x4c, 0x12, 0x25, 0x0c,
    0x70, 0x8d, 0x03, 0x29, 0xa0, 0xe1, 0x53, 0x24, 0xec, 0x13, 0xd9, 0xee,
    0x19, 0xbf, 0x10, 0xb3, 0x4a, 0x8c, 0x3f, 0x89, 0xa3, 0x61, 0x51, 0xde,
    0xac, 0x87, 0x07, 0x94, 0xf4, 0x63, 0x71, 0xec, 0x2e, 0xe2, 0x6f, 0x5b,
    0x98, 0x81, 0xe1, 0x89, 0x5c, 0x34, 0x79, 0x6c, 0x76, 0xef, 0x3b, 0x90,
    0x62, 0x79, 0xe6, 0xdb, 0xa4, 0x9a, 0x2f, 0x26, 0xc5, 0xd0, 0x10, 0xe1,
    0x0e, 0xde, 0xd9, 0x10, 0x8e, 0x16, 0xfb, 0xb7, 0xf7, 0xa8, 0xf7, 0xc7,
    0xe5, 0x02, 0x07, 0x98, 0x8f, 0x36, 0x08, 0x95, 0xe7, 0xe2, 0x37, 0x96,
    0x0d, 0x36, 0x75, 0x9e, 0xfb, 0x0e, 0x72, 0xb1, 0x1d, 0x9b, 0xbc, 0x03,
    0xf9, 0x49, 0x05, 0xd8, 0x81, 0xdd, 0x05, 0xb4, 0x2a, 0xd6, 0x41, 0xe9,
    0xac, 0x01, 0x76, 0x95, 0x0a, 0x0f, 0xd8, 0xdf, 0xd5, 0xbd, 0x12, 0x1f,
    0x35, 0x2f, 0x28, 0x17, 0x6c, 0xd2, 0x98, 0xc1, 0xa8, 0x09, 0x64, 0x77,
    0x6e, 0x47, 0x37, 0xba, 0xce, 0xac, 0x59, 0x5e, 0x68, 0x9d, 0x7f, 0x72,
    0xd6, 0x89, 0xc5, 0x06, 0x41, 0x29, 0x3e, 0x59, 0x3e, 0xdd, 0x26, 0xf5,
    0x24, 0xc9, 0x11, 0xa7, 0x5a, 0xa3, 0x4c, 0x40, 0x1f, 0x46, 0xa1, 0x99,
    0xb5, 0xa7, 0x3a, 0x51, 0x6e, 0x86, 0x3b, 0x9e, 0x7d, 0x72, 0xa7, 0x12,
    0x05, 0x78, 0x59, 0xed, 0x3e, 0x51, 0x78, 0x15, 0x0b, 0x03, 0x8f, 0x8d,
    0xd0, 0x2f, 0x05, 0xb2, 0x3e, 0x7b, 0x4a, 0x1c, 0x4b, 0x73, 0x05, 0x12,
    0xfc, 0xc6, 0xea, 0xe0, 0x50, 0x13, 0x7c, 0x43, 0x93, 0x74, 0xb3, 0xca,
    0x74, 0xe7, 0x8e, 0x1f, 0x01, 0x08, 0xd0, 0x30, 0xd4, 0x5b, 0x71, 0x36,
    0xb4, 0x07, 0xba, 0xc1, 0x30, 0x30, 0x5c, 0x48, 0xb7, 0x82, 0x3b, 0x98,
    0xa6, 0x7d, 0x60, 0x8a, 0xa2, 0xa3, 0x29, 0x82, 0xcc, 0xba, 0xbd, 0x83,
    0x04, 0x1b, 0xa2, 0x83, 0x03, 0x41, 0xa1, 0xd6, 0x05, 0xf1, 0x1b, 0xc2,
    0xb6, 0xf0, 0xa8, 0x7c, 0x86, 0x3b, 0x46, 0xa8, 0x48, 0x2a, 0x88, 0xdc,
    0x76, 0x9a, 0x76, 0xbf, 0x1f, 0x6a, 0xa5, 0x3d, 0x19, 0x8f, 0xeb, 0x38,
    0xf3, 0x64, 0xde, 0xc8, 0x2b, 0x0d, 0x0a, 0x28, 0xff, 0xf7, 0xdb, 0xe2,
    0x15, 0x42, 0xd4, 0x22, 0xd0, 0x27, 0x5d, 0xe1, 0x79, 0xfe, 0x18, 0xe7,
    0x70, 0x88, 0xad, 0x4e, 0xe6, 0xd9, 0x8b, 0x3a, 0xc6, 0xdd, 0x27, 0x51,
    0x6e, 0xff, 0xbc, 0x64, 0xf5, 0x33, 0x43, 0x4f};
#define TLS_ISRG_X1_EXPONENT 65537

static const p8 tls_usertrust_ecc_x[48] = {
    0x1a, 0xac, 0x54, 0x5a, 0xa9, 0xf9, 0x68, 0x23, 0xe7, 0x7a, 0xd5, 0x24,
    0x6f, 0x53, 0xc6, 0x5a, 0xd8, 0x4b, 0xab, 0xc6, 0xd5, 0xb6, 0xd1, 0xe6,
    0x73, 0x71, 0xae, 0xdd, 0x9c, 0xd6, 0x0c, 0x61, 0xfd, 0xdb, 0xa0, 0x89,
    0x03, 0xb8, 0x05, 0x14, 0xec, 0x57, 0xce, 0xee, 0x5d, 0x3f, 0xe2, 0x21};
static const p8 tls_usertrust_ecc_y[48] = {
    0xb3, 0xce, 0xf7, 0xd4, 0x8a, 0x79, 0xe0, 0xa3, 0x83, 0x7e, 0x2d, 0x97,
    0xd0, 0x61, 0xc4, 0xf1, 0x99, 0xdc, 0x25, 0x91, 0x63, 0xab, 0x7f, 0x30,
    0xa3, 0xb4, 0x70, 0xe2, 0xc7, 0xa1, 0x33, 0x9c, 0xf3, 0xbf, 0x2e, 0x5c,
    0x53, 0xb1, 0x5f, 0xb3, 0x7d, 0x32, 0x7f, 0x8a, 0x34, 0xe3, 0x79, 0x79};

static const p8 tls_oid_ec[7] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const p8 tls_oid_p256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const p8 tls_oid_p384[5] = {0x2b, 0x81, 0x04, 0x00, 0x22};
static const p8 tls_oid_ecdsa_sha256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04,
                                           0x03, 0x02};
static const p8 tls_oid_ecdsa_sha384[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04,
                                           0x03, 0x03};
static const p8 tls_oid_sha256_rsa[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
                                         0x01, 0x01, 0x0b};
static const p8 tls_oid_rsa[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
                                  0x01, 0x01};
static const p8 tls_oid_san[3] = {0x55, 0x1d, 0x11};
static const p8 tls_oid_basic_constraints[3] = {0x55, 0x1d, 0x13};
static const p8 tls_oid_name_constraints[3] = {0x55, 0x1d, 0x1e};
static const p8 tls_oid_key_usage[3] = {0x55, 0x1d, 0x0f};
static const p8 tls_oid_extended_key_usage[3] = {0x55, 0x1d, 0x25};
static const p8 tls_oid_server_auth[8] = {0x2b, 0x06, 0x01, 0x05,
                                          0x05, 0x07, 0x03, 0x01};

typedef struct
{
        bipolar handle;
        bool check_cert;
        bool encrypted;
        bool application;
        string_address host;
        crypto_sha256 transcript;
        p8 scalar[32];
        p8 hs_secret[32];
        p8 c_hs_traffic[32];
        p8 s_hs_traffic[32];
        p8 c_ap_traffic[32];
        p8 s_ap_traffic[32];
        p8 c_key[16];
        p8 s_key[16];
        p8 c_iv[12];
        p8 s_iv[12];
        p64 seq_read;
        p64 seq_write;
        p8 leftover[TLS_RECORD_MAX];
        positive leftover_used;
        p8 leaf_qx[48];
        p8 leaf_qy[48];
        p8 leaf_n[512];
        positive leaf_n_length;
        p64 leaf_e;
        p8 leaf_curve;
} tls_conn;

/* RFC 8446 requires each AEAD key to stay within its usage bound.  This
   client intentionally does not implement KeyUpdate, so end the connection
   before AES-GCM reaches the 2^24.5-record analysis bound.  The conservative
   integer limit also makes sequence wrap unreachable. */
#define TLS_AES_GCM_RECORD_LIMIT ((p64)1 << 24)

static fn tls_forget(tls_conn address_to tls)
{
        crypto_forget(tls, sizeof(*tls));
        tls->handle = -1;
}

static fn tls_expand_label(p8 address_to secret, string_address label,
                           p8 address_to context, positive context_length,
                           p8 address_to out, positive out_length)
{
        p8 info[256];
        positive label_length = string_length(label);
        positive used = 2;

        info[0] = (p8)(out_length >> 8);
        info[1] = (p8)out_length;
        info[used++] = (p8)(6 + label_length);
        memory_copy(info + used, "tls13 ", 6);
        used += 6;
        memory_copy(info + used, label, label_length);
        used += label_length;
        info[used++] = (p8)context_length;
        if (context_length)
        {
                memory_copy(info + used, context, context_length);
                used += context_length;
        }

        crypto_hkdf_expand(secret, info, used, out, out_length);
        crypto_forget(info, sizeof info);
}

static fn tls_derive_secret(p8 address_to secret, string_address label,
                            crypto_sha256 address_to transcript,
                            p8 address_to out)
{
        crypto_sha256 copy = *transcript;
        p8 hash[32];

        crypto_sha256_close(address_of copy, hash);
        tls_expand_label(secret, label, hash, 32, out, 32);
        crypto_forget(address_of copy, sizeof copy);
        crypto_forget(hash, sizeof hash);
}

static fn tls_empty_hash(p8 address_to out)
{
        crypto_sha256 hash;

        crypto_sha256_open(address_of hash);
        crypto_sha256_close(address_of hash, out);
        crypto_forget(address_of hash, sizeof hash);
}

static fn tls_traffic_keys(p8 address_to traffic, p8 address_to key,
                           p8 address_to iv)
{
        tls_expand_label(traffic, "key", null, 0, key, 16);
        tls_expand_label(traffic, "iv", null, 0, iv, 12);
}

static fn tls_nonce(p8 address_to iv, p64 seq, p8 address_to nonce)
{
        p8 seq_bytes[12];
        positive i;

        memory_fill(seq_bytes, 0, 12);
        crypto_put_be64(seq_bytes + 4, seq);
        for (i = 0; i < 12; i++)
                nonce[i] = iv[i] ^ seq_bytes[i];
        crypto_forget(seq_bytes, sizeof seq_bytes);
}

static bipolar tls_send_plain(tls_conn address_to tls, p8 type, p8 address_to body,
                              positive length)
{
        p8 header[5];

        header[0] = type;
        header[1] = 0x03;
        header[2] = 0x03;
        header[3] = (p8)(length >> 8);
        header[4] = (p8)length;
        if (!network_stream_send_all(tls->handle, header, 5))
                return TLS_FAIL;
        return network_stream_send_all(tls->handle, body, length) ? TLS_OK
                                                                  : TLS_FAIL;
}

static bipolar tls_send_enc(tls_conn address_to tls, p8 inner_type,
                            p8 address_to body, positive length)
{
        p8 inner[TLS_RECORD_MAX];
        p8 header[5];
        p8 nonce[12];
        p8 tag[16];
        p8 aad[5];
        positive inner_length = 0;
        positive record = 0;
        bipolar status = TLS_FAIL;

        if (length > TLS_RECORD_MAX - 17 ||
            tls->seq_write >= TLS_AES_GCM_RECORD_LIMIT)
                goto done;
        inner_length = length + 1;
        record = inner_length + 16;

        memory_copy(inner, body, length);
        inner[length] = inner_type;

        header[0] = TLS_CT_APP;
        header[1] = 0x03;
        header[2] = 0x03;
        header[3] = (p8)(record >> 8);
        header[4] = (p8)record;
        memory_copy(aad, header, 5);

        tls_nonce(tls->c_iv, tls->seq_write, nonce);
        crypto_aesgcm_encrypt(tls->c_key, nonce, aad, 5, inner, inner_length,
                              tag);
        tls->seq_write++;

        if (!network_stream_send_all(tls->handle, header, 5))
                goto done;
        if (!network_stream_send_all(tls->handle, inner, inner_length))
                goto done;
        status = network_stream_send_all(tls->handle, tag, 16) ? TLS_OK
                                                               : TLS_FAIL;

done:
        if (inner_length)
                crypto_forget(inner, inner_length);
        crypto_forget(header, sizeof header);
        crypto_forget(nonce, sizeof nonce);
        crypto_forget(tag, sizeof tag);
        crypto_forget(aad, sizeof aad);
        return status;
}

static bipolar tls_decrypt_record(tls_conn address_to tls, p8 address_to payload,
                                  positive payload_length, p8 address_to aad,
                                  p8 address_to inner, positive address_to inner_length,
                                  p8 address_to type)
{
        p8 nonce[12];
        p8 tag[16];
        positive at = 0;
        bipolar status = TLS_FAIL;

        if (payload_length < 16 ||
            tls->seq_read >= TLS_AES_GCM_RECORD_LIMIT)
                goto done;

        memory_copy(inner, payload, payload_length - 16);
        memory_copy(tag, payload + payload_length - 16, 16);
        tls_nonce(tls->s_iv, tls->seq_read, nonce);
        if (!crypto_aesgcm_decrypt(tls->s_key, nonce, aad, 5, inner,
                                   payload_length - 16, tag))
                goto done;

        tls->seq_read++;
        at = payload_length - 16;
        while (at && inner[at - 1] == 0)
                at--;
        if (!at)
                goto done;
        address_to type = inner[at - 1];
        address_to inner_length = at - 1;
        status = TLS_OK;

done:
        if (status && payload_length >= 16)
                crypto_forget(inner, payload_length - 16);
        crypto_forget(nonce, sizeof nonce);
        crypto_forget(tag, sizeof tag);
        return status;
}

static bool tls_compatibility_ccs_valid(p8 address_to payload,
                                        positive length,
                                        bool application)
{
        return !application && length == 1 && payload[0] == 1;
}

static bool tls_compatibility_ccs_take(bool address_to seen)
{
        if (*seen)
                return false;
        *seen = true;
        return true;
}

static bool tls_record_version_valid(p8 address_to header)
{
        return header[1] == 0x03 && header[2] == 0x03;
}

static bipolar tls_read_record(tls_conn address_to tls, p8 address_to type,
                               p8 address_to body, positive room,
                               positive address_to length,
                               const network_deadline address_to deadline)
{
        p8 header[5];
        p8 payload[TLS_RECORD_MAX];
        positive payload_length = 0;
        p8 inner[TLS_RECORD_MAX];
        positive inner_length = 0;
        p8 inner_type = 0;
        bipolar status = TLS_FAIL;

        if (!network_stream_read_all(tls->handle, header, 5, deadline))
                goto done;

        /* TLS 1.3 authenticates these bytes as AAD for encrypted records and
           fixes legacy_record_version at TLS 1.2 for every server record. */
        if (!tls_record_version_valid(header))
        {
                status = TLS_FAIL;
                goto done;
        }

        payload_length = ((positive)header[3] << 8) | header[4];
        if (!payload_length || payload_length > TLS_RECORD_MAX)
        {
                status = TLS_FAIL;
                goto done;
        }
        if (!network_stream_read_all(tls->handle, payload, payload_length,
                                     deadline))
        {
                status = TLS_FAIL;
                goto done;
        }

        if (header[0] == TLS_CT_CCS)
        {
                if (!tls_compatibility_ccs_valid(payload, payload_length,
                                                 tls->application))
                {
                        status = TLS_FAIL;
                        goto done;
                }
                address_to type = TLS_CT_CCS;
                address_to length = 0;
                status = TLS_OK;
                goto done;
        }

        if (!tls->encrypted)
        {
                if (header[0] != TLS_CT_HANDSHAKE || payload_length > room)
                {
                        status = TLS_FAIL;
                        goto done;
                }
                memory_copy(body, payload, payload_length);
                address_to type = TLS_CT_HANDSHAKE;
                address_to length = payload_length;
                status = TLS_OK;
                goto done;
        }

        if (header[0] != TLS_CT_APP)
        {
                status = TLS_FAIL;
                goto done;
        }

        if (tls_decrypt_record(tls, payload, payload_length, header, inner,
                               address_of inner_length, address_of inner_type))
        {
                status = TLS_FAIL;
                goto done;
        }

        if (inner_type == TLS_CT_ALERT)
        {
                status = (inner_length == 2 && inner[1] == 0) ? TLS_EOF
                                                              : TLS_FAIL;
                goto done;
        }

        if (inner_length > room)
        {
                status = TLS_FAIL;
                goto done;
        }
        memory_copy(body, inner, inner_length);
        address_to type = inner_type;
        address_to length = inner_length;
        status = TLS_OK;

done:
        crypto_forget(header, sizeof header);
        if (payload_length <= TLS_RECORD_MAX)
                crypto_forget(payload, payload_length);
        if (payload_length >= 16 && payload_length <= TLS_RECORD_MAX)
                crypto_forget(inner, payload_length - 16);
        return status;
}

static fn tls_transcript_add(tls_conn address_to tls, p8 address_to msg,
                             positive length)
{
        crypto_sha256_write(address_of tls->transcript, msg, length);
}

static bipolar tls_asn1_length(p8 address_to bytes, positive size,
                               positive address_to at, positive address_to length)
{
        positive i = address_to at;
        p8 first;
        positive count;
        positive value;

        if (i >= size)
                return TLS_FAIL;

        first = bytes[i++];
        if (first < 0x80)
        {
                address_to length = first;
                address_to at = i;
                return TLS_OK;
        }

        count = first & 0x7f;
        value = 0;
        if (!count || count > 3 || i + count > size || !bytes[i])
                return TLS_FAIL;
        while (count)
        {
                value = (value << 8) | bytes[i++];
                count--;
        }
        if (value < 0x80)
                return TLS_FAIL;
        address_to length = value;
        address_to at = i;
        return TLS_OK;
}

static bipolar tls_asn1_enter(p8 address_to bytes, positive size, p8 tag,
                              positive address_to at, positive address_to stop)
{
        positive i = address_to at;
        positive length = 0;

        if (i >= size || bytes[i] != tag)
                return TLS_FAIL;
        i++;
        address_to at = i;
        if (tls_asn1_length(bytes, size, at, address_of length))
                return TLS_FAIL;
        if (address_to at + length > size)
                return TLS_FAIL;
        address_to stop = address_to at + length;
        return TLS_OK;
}

static bipolar tls_asn1_skip(p8 address_to bytes, positive size, positive address_to at)
{
        positive length = 0;
        positive i = address_to at;

        if (i >= size)
                return TLS_FAIL;
        i++;
        address_to at = i;
        if (tls_asn1_length(bytes, size, at, address_of length))
                return TLS_FAIL;
        if (address_to at + length > size)
                return TLS_FAIL;
        address_to at += length;
        return TLS_OK;
}

static bool tls_oid_is(p8 address_to bytes, positive length, p8 address_to oid,
                       positive oid_length)
{
        return length == oid_length && !memory_compare(bytes, oid, oid_length);
}

/* DER INTEGERs used for keys and ECDSA signatures are strictly positive and
   minimally encoded.  Return the magnitude without the one permitted sign
   octet. */
static bool tls_positive_integer(p8 address_to bytes, positive at,
                                 positive stop, positive address_to value_at,
                                 positive address_to value_length)
{
        if (at >= stop || (bytes[at] & 0x80))
                return false;

        if (!bytes[at])
        {
                if (at + 1 >= stop || !(bytes[at + 1] & 0x80))
                        return false;
                at++;
        }

        address_to value_at = at;
        address_to value_length = stop - at;
        return true;
}

/* The verifier implements these three certificate signature algorithms.
   ECDSA parameters must be absent; RSA's historical NULL may be present or
   absent, but no other parameter or trailing value is accepted. */
static bool tls_signature_algorithm(p8 address_to der, positive size,
                                    positive address_to at,
                                    p8 address_to address_to oid,
                                    positive address_to oid_length)
{
        positive alg_stop = 0;
        positive oid_at;
        positive oid_stop = 0;
        bool ecdsa;
        bool rsa;

        if (tls_asn1_enter(der, size, 0x30, at, address_of alg_stop))
                return false;
        oid_at = address_to at;
        if (tls_asn1_enter(der, alg_stop, 0x06, address_of oid_at,
                           address_of oid_stop))
                return false;

        ecdsa = tls_oid_is(der + oid_at, oid_stop - oid_at,
                           tls_oid_ecdsa_sha256, sizeof tls_oid_ecdsa_sha256) ||
                tls_oid_is(der + oid_at, oid_stop - oid_at,
                           tls_oid_ecdsa_sha384, sizeof tls_oid_ecdsa_sha384);
        rsa = tls_oid_is(der + oid_at, oid_stop - oid_at,
                         tls_oid_sha256_rsa, sizeof tls_oid_sha256_rsa);
        if (!ecdsa && !rsa)
                return false;
        if (ecdsa && oid_stop != alg_stop)
                return false;
        if (rsa && oid_stop != alg_stop &&
            (oid_stop + 2 != alg_stop || der[oid_stop] != 0x05 ||
             der[oid_stop + 1] != 0))
                return false;

        address_to oid = der + oid_at;
        address_to oid_length = oid_stop - oid_at;
        address_to at = alg_stop;
        return true;
}

static bool tls_host_match(string_address host, p8 address_to name,
                           positive name_length)
{
        positive host_length = string_length(host);
        string_address star;

        if (name_length == host_length &&
            !memory_compare_ascii_case(name, host, host_length))
                return true;

        if (!name_length || name[0] != '*' || name_length < 3 || name[1] != '.')
                return false;

        star = string_first_of(host, '.');
        if (!star || !star[1])
                return false;

        return string_length(star) == name_length - 1 &&
               !memory_compare_ascii_case(star, (string_address)(name + 1),
                                          name_length - 1);
}

static bool tls_general_name_match(string_address host, p8 tag,
                                   p8 address_to name, positive name_length)
{
        bipolar address = string_to_host(host);

        if (address >= 0)
                return tag == 0x87 && name_length == 4 &&
                       network_load_32(name) == (p32)address;

        return tag == 0x82 && tls_host_match(host, name, name_length);
}

/* Parse the signed GeneralNames value once, all the way to its declared end.
   A matching first entry does not authorize the certificate until every
   later entry and the outer DER framing have also been validated. */
static bool tls_parse_san(p8 address_to value, positive length,
                          string_address host, bool address_to matched)
{
        positive at = 0;
        positive stop = 0;
        bool found = false;

        if (tls_asn1_enter(value, length, 0x30, address_of at,
                           address_of stop) ||
            stop != length || at == stop)
                return false;

        while (at < stop)
        {
                positive name_stop = 0;
                p8 tag = value[at];

                if (tag != 0xa0 && tag != 0x81 && tag != 0x82 &&
                    tag != 0xa3 && tag != 0xa4 && tag != 0xa5 &&
                    tag != 0x86 && tag != 0x87 && tag != 0x88)
                        return false;
                if (tls_asn1_enter(value, stop, tag, address_of at,
                                   address_of name_stop))
                        return false;
                if (tag == 0x81 || tag == 0x82 || tag == 0x86)
                        for (positive byte = at; byte < name_stop; byte++)
                                if (value[byte] < 0x20 || value[byte] >= 0x7f)
                                        return false;
                if (tag == 0x87 && name_stop - at != 4 &&
                    name_stop - at != 16)
                        return false;
                if (host && tls_general_name_match(host, tag, value + at,
                                                   name_stop - at))
                        found = true;
                at = name_stop;
        }

        if (matched)
                address_to matched = found;
        return at == stop;
}

static bipolar tls_parse_ecdsa_sig(p8 address_to sig, positive length,
                                   p8 address_to r, positive address_to r_length,
                                   p8 address_to s, positive address_to s_length)
{
        positive at = 0;
        positive stop = 0;
        positive r_stop = 0;
        positive s_stop = 0;
        positive value_at;
        positive value_length;

        if (tls_asn1_enter(sig, length, 0x30, address_of at, address_of stop) ||
            stop != length)
                return TLS_FAIL;
        if (tls_asn1_enter(sig, stop, 0x02, address_of at, address_of r_stop))
                return TLS_FAIL;
        if (!tls_positive_integer(sig, at, r_stop, address_of value_at,
                                  address_of value_length) ||
            value_length > 48)
                return TLS_FAIL;
        address_to r_length = value_length;
        memory_copy(r, sig + value_at, value_length);
        at = r_stop;
        if (tls_asn1_enter(sig, stop, 0x02, address_of at, address_of s_stop))
                return TLS_FAIL;
        if (!tls_positive_integer(sig, at, s_stop, address_of value_at,
                                  address_of value_length) ||
            value_length > 48)
                return TLS_FAIL;
        address_to s_length = value_length;
        memory_copy(s, sig + value_at, value_length);
        return s_stop == stop ? TLS_OK : TLS_FAIL;
}

typedef struct
{
        p8 address_to tbs;
        positive tbs_length;
        p8 address_to issuer;
        positive issuer_length;
        p8 address_to subject;
        positive subject_length;
        p8 address_to sig_oid;
        positive sig_oid_length;
        p8 address_to sig;
        positive sig_length;
        p8 curve;
        p8 qx[48];
        p8 qy[48];
        p8 modulus[512];
        positive modulus_length;
        p64 exponent;
        p64 not_before;
        p64 not_after;
        positive path_length;
        bool basic_constraints;
        bool ca;
        bool path_length_present;
        bool key_usage;
        bool digital_signature;
        bool key_cert_sign;
        bool extended_key_usage;
        bool server_auth;
        bool san;
        bool san_match;
        bool unsupported_critical;
} tls_cert;

static bool tls_date_value(p8 tag, p8 address_to text, positive length,
                           p64 address_to value)
{
        static const p8 days_in_month[12] = {31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31};
        positive year_digits;
        positive year = 0;
        positive month;
        positive day;
        positive hour;
        positive minute;
        positive second;
        positive i;
        positive limit;
        p64 answer;

        if (tag == 0x17)
                year_digits = 2;
        else if (tag == 0x18)
                year_digits = 4;
        else
                return false;
        if (length != year_digits + 11 || text[length - 1] != 'Z')
                return false;
        for (i = 0; i + 1 < length; i++)
                if (text[i] < '0' || text[i] > '9')
                        return false;

        for (i = 0; i < year_digits; i++)
                year = year * 10 + text[i] - '0';
        if (year_digits == 2)
                year += year >= 50 ? 1900 : 2000;
        if (!year)
                return false;

        month = (positive)(text[year_digits] - '0') * 10 +
                text[year_digits + 1] - '0';
        day = (positive)(text[year_digits + 2] - '0') * 10 +
              text[year_digits + 3] - '0';
        hour = (positive)(text[year_digits + 4] - '0') * 10 +
               text[year_digits + 5] - '0';
        minute = (positive)(text[year_digits + 6] - '0') * 10 +
                 text[year_digits + 7] - '0';
        second = (positive)(text[year_digits + 8] - '0') * 10 +
                 text[year_digits + 9] - '0';
        if (!month || month > 12 || !day || hour > 23 || minute > 59 ||
            second > 59)
                return false;
        limit = days_in_month[month - 1];
        if (month == 2 && (!(year % 4) && (year % 100 || !(year % 400))))
                limit++;
        if (day > limit)
                return false;

        answer = year;
        answer = answer * 100 + month;
        answer = answer * 100 + day;
        answer = answer * 100 + hour;
        answer = answer * 100 + minute;
        answer = answer * 100 + second;
        address_to value = answer;
        return true;
}

static bipolar tls_parse_validity(p8 address_to der, positive size,
                                  positive address_to at, tls_cert address_to cert)
{
        positive validity_stop = 0;
        positive time_stop = 0;
        p8 tag;

        if (tls_asn1_enter(der, size, 0x30, at, address_of validity_stop) ||
            address_to at >= validity_stop)
                return TLS_FAIL;
        tag = der[address_to at];
        if (tls_asn1_enter(der, validity_stop, tag, at, address_of time_stop) ||
            !tls_date_value(tag, der + address_to at, time_stop - address_to at,
                            address_of cert->not_before))
                return TLS_FAIL;
        address_to at = time_stop;
        if (address_to at >= validity_stop)
                return TLS_FAIL;
        tag = der[address_to at];
        if (tls_asn1_enter(der, validity_stop, tag, at, address_of time_stop) ||
            !tls_date_value(tag, der + address_to at, time_stop - address_to at,
                            address_of cert->not_after) ||
            time_stop != validity_stop || cert->not_after < cert->not_before)
                return TLS_FAIL;
        address_to at = validity_stop;
        return TLS_OK;
}

static bipolar tls_parse_basic_constraints(p8 address_to value, positive length,
                                            tls_cert address_to cert)
{
        positive at = 0;
        positive stop = 0;

        if (tls_asn1_enter(value, length, 0x30, address_of at, address_of stop) ||
            stop != length)
                return TLS_FAIL;
        if (at < stop && value[at] == 0x01)
        {
                positive boolean_stop = 0;

                if (tls_asn1_enter(value, stop, 0x01, address_of at,
                                   address_of boolean_stop) ||
                    at + 1 != boolean_stop ||
                    (value[at] != 0 && value[at] != 0xff))
                        return TLS_FAIL;
                cert->ca = value[at] != 0;
                at = boolean_stop;
        }
        if (at < stop && value[at] == 0x02)
        {
                positive integer_stop = 0;
                positive bytes;
                positive path = 0;

                if (tls_asn1_enter(value, stop, 0x02, address_of at,
                                   address_of integer_stop))
                        return TLS_FAIL;
                bytes = integer_stop - at;
                if (!bytes || bytes > sizeof(positive) || (value[at] & 0x80) ||
                    (bytes > 1 && !value[at] && !(value[at + 1] & 0x80)))
                        return TLS_FAIL;
                while (at < integer_stop)
                        path = (path << 8) | value[at++];
                cert->path_length = path;
                cert->path_length_present = true;
        }
        if (at != stop || (cert->path_length_present && !cert->ca))
                return TLS_FAIL;
        return TLS_OK;
}

static bipolar tls_parse_key_usage(p8 address_to value, positive length,
                                   tls_cert address_to cert)
{
        positive at = 0;
        positive stop = 0;
        p8 unused;

        if (tls_asn1_enter(value, length, 0x03, address_of at, address_of stop) ||
            stop != length || at >= stop)
                return TLS_FAIL;
        unused = value[at++];
        if (unused > 7 || at >= stop ||
            (unused && (value[stop - 1] & (((p8)1 << unused) - 1))))
                return TLS_FAIL;
        cert->digital_signature = (value[at] & 0x80) != 0;
        cert->key_cert_sign = (value[at] & 0x04) != 0;
        return TLS_OK;
}

static bipolar tls_parse_extended_key_usage(p8 address_to value, positive length,
                                            tls_cert address_to cert)
{
        positive at = 0;
        positive stop = 0;

        if (tls_asn1_enter(value, length, 0x30, address_of at, address_of stop) ||
            stop != length || at == stop)
                return TLS_FAIL;
        while (at < stop)
        {
                positive oid_stop = 0;

                if (tls_asn1_enter(value, stop, 0x06, address_of at,
                                   address_of oid_stop))
                        return TLS_FAIL;
                if (tls_oid_is(value + at, oid_stop - at, tls_oid_server_auth, 8))
                        cert->server_auth = true;
                at = oid_stop;
        }
        return TLS_OK;
}

static bipolar tls_parse_extensions(p8 address_to der, positive tbs_stop,
                                    positive at, tls_cert address_to cert,
                                    string_address host)
{
        bool issuer_unique = false;
        bool subject_unique = false;
        positive extensions_stop = 0;
        positive sequence_stop = 0;

        while (at < tbs_stop && (der[at] == 0x81 || der[at] == 0x82))
        {
                bool address_to seen = der[at] == 0x81 ? address_of issuer_unique
                                                       : address_of subject_unique;

                if (address_to seen || tls_asn1_skip(der, tbs_stop, address_of at))
                        return TLS_FAIL;
                address_to seen = true;
        }
        if (at == tbs_stop)
                return TLS_OK;
        if (tls_asn1_enter(der, tbs_stop, 0xa3, address_of at,
                           address_of extensions_stop) ||
            extensions_stop != tbs_stop ||
            tls_asn1_enter(der, extensions_stop, 0x30, address_of at,
                           address_of sequence_stop) ||
            sequence_stop != extensions_stop)
                return TLS_FAIL;

        while (at < sequence_stop)
        {
                positive extension_stop = 0;
                positive oid_at;
                positive oid_stop = 0;
                positive value_stop = 0;
                bool critical = false;

                if (tls_asn1_enter(der, sequence_stop, 0x30, address_of at,
                                   address_of extension_stop))
                        return TLS_FAIL;
                oid_at = at;
                if (tls_asn1_enter(der, extension_stop, 0x06, address_of oid_at,
                                   address_of oid_stop))
                        return TLS_FAIL;
                at = oid_stop;
                if (at < extension_stop && der[at] == 0x01)
                {
                        positive boolean_stop = 0;

                        if (tls_asn1_enter(der, extension_stop, 0x01, address_of at,
                                           address_of boolean_stop) ||
                            at + 1 != boolean_stop ||
                            (der[at] != 0 && der[at] != 0xff))
                                return TLS_FAIL;
                        critical = der[at] != 0;
                        at = boolean_stop;
                }
                if (tls_asn1_enter(der, extension_stop, 0x04, address_of at,
                                   address_of value_stop) ||
                    value_stop != extension_stop)
                        return TLS_FAIL;

                if (tls_oid_is(der + oid_at, oid_stop - oid_at,
                               tls_oid_basic_constraints, 3))
                {
                        if (cert->basic_constraints ||
                            tls_parse_basic_constraints(der + at, value_stop - at,
                                                        cert))
                                return TLS_FAIL;
                        cert->basic_constraints = true;
                }
                else if (tls_oid_is(der + oid_at, oid_stop - oid_at,
                                    tls_oid_key_usage, 3))
                {
                        if (cert->key_usage ||
                            tls_parse_key_usage(der + at, value_stop - at, cert))
                                return TLS_FAIL;
                        cert->key_usage = true;
                }
                else if (tls_oid_is(der + oid_at, oid_stop - oid_at,
                                    tls_oid_extended_key_usage, 3))
                {
                        if (cert->extended_key_usage ||
                            tls_parse_extended_key_usage(der + at, value_stop - at,
                                                         cert))
                                return TLS_FAIL;
                        cert->extended_key_usage = true;
                }
                else if (tls_oid_is(der + oid_at, oid_stop - oid_at, tls_oid_san, 3))
                {
                        bool matched = false;

                        if (cert->san ||
                            !tls_parse_san(der + at, value_stop - at, host,
                                           address_of matched))
                                return TLS_FAIL;
                        cert->san = true;
                        cert->san_match = matched;
                }
                else if (tls_oid_is(der + oid_at, oid_stop - oid_at,
                                    tls_oid_name_constraints, 3))
                {
                        /* Namespace limits apply even when an issuer marks
                           them non-critical.  Until they are implemented,
                           accepting the chain would authorize names outside
                           the issuer's permitted subtrees. */
                        return TLS_FAIL;
                }
                else if (critical)
                        cert->unsupported_critical = true;

                at = extension_stop;
        }
        return TLS_OK;
}

static bipolar tls_parse_cert(p8 address_to der, positive length,
                              tls_cert address_to cert, string_address host)
{
        positive at = 0;
        positive stop = 0;
        positive tbs_stop = 0;
        positive spki_stop = 0;
        positive alg_stop = 0;
        positive oid_stop = 0;
        positive param_at;
        positive bit_stop = 0;

        memory_fill(cert, 0, sizeof(*cert));

        if (tls_asn1_enter(der, length, 0x30, address_of at, address_of stop) ||
            stop != length)
                return TLS_FAIL;
        cert->tbs = der + at;
        if (tls_asn1_enter(der, length, 0x30, address_of at, address_of tbs_stop))
                return TLS_FAIL;
        cert->tbs_length = (positive)((der + tbs_stop) - cert->tbs);

        at = tbs_stop;
        if (!tls_signature_algorithm(der, stop, address_of at,
                                     address_of cert->sig_oid,
                                     address_of cert->sig_oid_length))
                return TLS_FAIL;
        if (tls_asn1_enter(der, stop, 0x03, address_of at, address_of bit_stop) ||
            bit_stop != stop)
                return TLS_FAIL;
        if (at >= bit_stop || der[at] != 0)
                return TLS_FAIL;
        at++;
        cert->sig = der + at;
        cert->sig_length = bit_stop - at;

        at = (positive)(cert->tbs - der);
        if (tls_asn1_enter(der, length, 0x30, address_of at, address_of tbs_stop))
                return TLS_FAIL;
        if (at < tbs_stop && der[at] == 0xa0 &&
            tls_asn1_skip(der, tbs_stop, address_of at))
                return TLS_FAIL;
        if (tls_asn1_skip(der, tbs_stop, address_of at))
                return TLS_FAIL;
        {
                p8 address_to tbs_oid;
                positive tbs_oid_length;

                if (!tls_signature_algorithm(der, tbs_stop, address_of at,
                                             address_of tbs_oid,
                                             address_of tbs_oid_length) ||
                    tbs_oid_length != cert->sig_oid_length ||
                    memory_compare(tbs_oid, cert->sig_oid, tbs_oid_length))
                        return TLS_FAIL;
        }
        {
                positive name_at = at;

                if (tls_asn1_skip(der, tbs_stop, address_of at))
                        return TLS_FAIL;
                cert->issuer = der + name_at;
                cert->issuer_length = at - name_at;
        }
        if (tls_parse_validity(der, tbs_stop, address_of at, cert))
                return TLS_FAIL;
        {
                positive name_at = at;

                if (tls_asn1_skip(der, tbs_stop, address_of at))
                        return TLS_FAIL;
                cert->subject = der + name_at;
                cert->subject_length = at - name_at;
        }
        if (!cert->issuer_length || !cert->subject_length)
                return TLS_FAIL;

        if (tls_asn1_enter(der, tbs_stop, 0x30, address_of at, address_of spki_stop))
                return TLS_FAIL;
        if (tls_asn1_enter(der, spki_stop, 0x30, address_of at, address_of alg_stop))
                return TLS_FAIL;
        param_at = at;
        if (tls_asn1_enter(der, alg_stop, 0x06, address_of param_at, address_of oid_stop))
                return TLS_FAIL;

        if (tls_oid_is(der + param_at, oid_stop - param_at, tls_oid_ec, 7))
        {
                positive curve_stop = 0;
                positive coord;

                if (tls_asn1_enter(der, alg_stop, 0x06, address_of oid_stop,
                                   address_of curve_stop) ||
                    curve_stop != alg_stop)
                        return TLS_FAIL;
                if (tls_oid_is(der + oid_stop, curve_stop - oid_stop, tls_oid_p256, 8))
                        cert->curve = 1;
                else if (tls_oid_is(der + oid_stop, curve_stop - oid_stop,
                                    tls_oid_p384, 5))
                        cert->curve = 2;
                else
                        return TLS_FAIL;

                at = alg_stop;
                if (tls_asn1_enter(der, spki_stop, 0x03, address_of at,
                                   address_of bit_stop) ||
                    bit_stop != spki_stop)
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0)
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0x04)
                        return TLS_FAIL;
                coord = cert->curve == 1 ? 32 : 48;
                if (at + coord * 2 != bit_stop)
                        return TLS_FAIL;
                memory_copy(cert->qx + (48 - coord), der + at, coord);
                memory_copy(cert->qy + (48 - coord), der + at + coord, coord);
        }
        else if (tls_oid_is(der + param_at, oid_stop - param_at, tls_oid_rsa, 9))
        {
                positive n_stop = 0;
                positive e_stop = 0;
                positive rsa_stop = 0;
                positive value_at;
                positive value_length;

                if (oid_stop != alg_stop &&
                    (oid_stop + 2 != alg_stop || der[oid_stop] != 0x05 ||
                     der[oid_stop + 1] != 0))
                        return TLS_FAIL;

                cert->curve = 3;
                at = alg_stop;
                if (tls_asn1_enter(der, spki_stop, 0x03, address_of at,
                                   address_of bit_stop) ||
                    bit_stop != spki_stop)
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0)
                        return TLS_FAIL;
                if (tls_asn1_enter(der, bit_stop, 0x30, address_of at,
                                   address_of rsa_stop) ||
                    rsa_stop != bit_stop)
                        return TLS_FAIL;
                if (tls_asn1_enter(der, rsa_stop, 0x02, address_of at,
                                   address_of n_stop))
                        return TLS_FAIL;
                if (!tls_positive_integer(der, at, n_stop,
                                          address_of value_at,
                                          address_of value_length))
                        return TLS_FAIL;
                cert->modulus_length = value_length;
                if (cert->modulus_length < 256 ||
                    cert->modulus_length > sizeof(cert->modulus) ||
                    (cert->modulus_length == 256 &&
                     !(der[value_at] & 0x80)) ||
                    !(der[value_at + value_length - 1] & 1))
                        return TLS_FAIL;
                memory_copy(cert->modulus, der + value_at,
                            cert->modulus_length);
                at = n_stop;
                if (tls_asn1_enter(der, rsa_stop, 0x02, address_of at,
                                   address_of e_stop))
                        return TLS_FAIL;
                if (!tls_positive_integer(der, at, e_stop,
                                          address_of value_at,
                                          address_of value_length) ||
                    value_length > sizeof(cert->exponent))
                        return TLS_FAIL;
                cert->exponent = 0;
                while (value_at < e_stop)
                        cert->exponent = (cert->exponent << 8) |
                                         der[value_at++];
                at = e_stop;
                if (at != rsa_stop || cert->exponent < 3 ||
                    !(cert->exponent & 1))
                        return TLS_FAIL;
        }
        else
                return TLS_FAIL;

        return tls_parse_extensions(der, tbs_stop, spki_stop, cert, host);
}

static bool tls_spki_is_x2(tls_cert address_to cert)
{
        return cert->curve == 2 &&
               !memory_compare(cert->qx, tls_isrg_x2_x, 48) &&
               !memory_compare(cert->qy, tls_isrg_x2_y, 48);
}

static bool tls_spki_is_x1(tls_cert address_to cert)
{
        return cert->curve == 3 && cert->modulus_length == 512 &&
               cert->exponent == TLS_ISRG_X1_EXPONENT &&
               !memory_compare(cert->modulus, tls_isrg_x1_n, 512);
}

static bool tls_spki_is_usertrust(tls_cert address_to cert)
{
        return cert->curve == 2 &&
               !memory_compare(cert->qx, tls_usertrust_ecc_x, 48) &&
               !memory_compare(cert->qy, tls_usertrust_ecc_y, 48);
}

static bool tls_spki_is_anchor(tls_cert address_to cert)
{
        return tls_spki_is_x2(cert) || tls_spki_is_x1(cert) ||
               tls_spki_is_usertrust(cert);
}

static bool tls_certificate_names_chain(const tls_cert address_to child,
                                        const tls_cert address_to issuer)
{
        /* The server provides an already ordered path.  A signature made by
           a reused CA key is not sufficient to choose which CA identity and
           constraints issued the child: issuer and subject Names must also
           identify the same certificate authority.  Exact DER comparison is
           deliberately fail closed; conforming issuers reproduce their
           subject Name in issued certificates. */
        return child->issuer && issuer->subject && child->issuer_length &&
               child->issuer_length == issuer->subject_length &&
               !memory_compare(child->issuer, issuer->subject,
                               child->issuer_length);
}

static bool tls_verify_one(tls_cert address_to child, tls_cert address_to issuer)
{
        p8 hash[48];
        p8 r[48];
        p8 s[48];
        positive r_length = 0;
        positive s_length = 0;
        p8 address_to qx = issuer->qx + (issuer->curve == 1 ? 16 : 0);
        p8 address_to qy = issuer->qy + (issuer->curve == 1 ? 16 : 0);

        if (tls_oid_is(child->sig_oid, child->sig_oid_length, tls_oid_ecdsa_sha384,
                       8))
        {
                crypto_sha384(child->tbs, child->tbs_length, hash);
                if (tls_parse_ecdsa_sig(child->sig, child->sig_length, r,
                                        address_of r_length, s, address_of s_length))
                        return false;
                if (issuer->curve == 2)
                        return crypto_ecdsa_p384(hash, 48, r, r_length, s, s_length,
                                                 qx, qy);
                if (issuer->curve == 1)
                        return crypto_ecdsa_p256(hash, 48, r, r_length, s, s_length,
                                                 qx, qy);
                return false;
        }

        if (tls_oid_is(child->sig_oid, child->sig_oid_length, tls_oid_ecdsa_sha256,
                       8))
        {
                crypto_sha256_of(child->tbs, child->tbs_length, hash);
                if (tls_parse_ecdsa_sig(child->sig, child->sig_length, r,
                                        address_of r_length, s, address_of s_length))
                        return false;
                if (issuer->curve == 1)
                        return crypto_ecdsa_p256(hash, 32, r, r_length, s, s_length,
                                                 qx, qy);
                if (issuer->curve == 2)
                        return crypto_ecdsa_p384(hash, 32, r, r_length, s, s_length,
                                                 qx, qy);
                return false;
        }

        if (tls_oid_is(child->sig_oid, child->sig_oid_length, tls_oid_sha256_rsa, 9))
        {
                crypto_sha256_of(child->tbs, child->tbs_length, hash);
                return issuer->curve == 3 &&
                       crypto_rsa_pkcs1_sha256(issuer->modulus, issuer->modulus_length,
                                               issuer->exponent, child->sig,
                                               child->sig_length, hash);
        }

        return false;
}

static fn tls_keep_leaf(tls_conn address_to tls, tls_cert address_to leaf)
{
        tls->leaf_curve = leaf->curve;
        tls->leaf_n_length = 0;
        tls->leaf_e = 0;
        memory_fill(tls->leaf_qx, 0, sizeof(tls->leaf_qx));
        memory_fill(tls->leaf_qy, 0, sizeof(tls->leaf_qy));
        memory_fill(tls->leaf_n, 0, sizeof(tls->leaf_n));
        if (leaf->curve == 3)
        {
                memory_copy(tls->leaf_n, leaf->modulus, leaf->modulus_length);
                tls->leaf_n_length = leaf->modulus_length;
                tls->leaf_e = leaf->exponent;
        }
        else
        {
                memory_copy(tls->leaf_qx, leaf->qx, 48);
                memory_copy(tls->leaf_qy, leaf->qy, 48);
        }
}

static bool tls_date_now(p64 address_to value)
{
        time_t stamp = time(null);
        tm calendar;
        p64 answer;

        if (stamp < 0 || !gmtime_r(address_of stamp, address_of calendar))
                return false;
        answer = (p64)(calendar.tm_year + 1900);
        answer = answer * 100 + (p64)(calendar.tm_mon + 1);
        answer = answer * 100 + (p64)calendar.tm_mday;
        answer = answer * 100 + (p64)calendar.tm_hour;
        answer = answer * 100 + (p64)calendar.tm_min;
        answer = answer * 100 + (p64)calendar.tm_sec;
        address_to value = answer;
        return true;
}

static bool tls_cert_current(tls_cert address_to cert, p64 now)
{
        return !cert->unsupported_critical && cert->not_before <= now &&
               now <= cert->not_after;
}

static bool tls_leaf_authorized(tls_cert address_to cert, p64 now)
{
        return tls_cert_current(cert, now) &&
               (!cert->key_usage || cert->digital_signature) &&
               (!cert->extended_key_usage || cert->server_auth);
}

static bool tls_issuer_authorized(tls_cert address_to cert, positive ca_below,
                                  p64 now)
{
        return tls_cert_current(cert, now) && cert->basic_constraints && cert->ca &&
               (!cert->key_usage || cert->key_cert_sign) &&
               (!cert->extended_key_usage || cert->server_auth) &&
               (!cert->path_length_present || ca_below <= cert->path_length);
}

static bool tls_certificate_body_open(p8 address_to body,
                                      positive body_length,
                                      positive address_to entries_at,
                                      positive address_to list_end)
{
        positive at = 1;
        positive list_length;

        /* The server Certificate in this initial handshake has an empty
           request_context.  Its three-byte list vector must consume the rest
           of the handshake body exactly. */
        if (body_length < 4 || body[0] != 0 || at + 3 > body_length)
                return false;

        list_length = ((positive)body[at] << 16) |
                      ((positive)body[at + 1] << 8) | body[at + 2];
        at += 3;
        if (list_length != body_length - at)
                return false;

        *entries_at = at;
        *list_end = at + list_length;
        return true;
}

static bool tls_verify_chain(p8 address_to body, positive body_length,
                             string_address host, tls_conn address_to tls)
{
        tls_cert certs[8];
        positive count = 0;
        positive at;
        positive list_end;
        positive i;
        p64 now = 0;

        if (!tls_certificate_body_open(body, body_length,
                                       address_of at, address_of list_end))
                return false;

        while (at + 3 <= list_end && count < 8)
        {
                positive cert_length = ((positive)body[at] << 16) |
                                       ((positive)body[at + 1] << 8) | body[at + 2];
                positive ext_length;

                at += 3;
                if (at + cert_length + 2 > list_end)
                        return false;
                if (tls_parse_cert(body + at, cert_length, certs + count,
                                   count ? null : host))
                        return false;
                at += cert_length;
                ext_length = ((positive)body[at] << 8) | body[at + 1];
                at += 2;
                if (at + ext_length > list_end)
                        return false;
                at += ext_length;
                count++;
        }

        if (!count || at != list_end)
                return false;

        tls_keep_leaf(tls, certs);

        if (tls->check_cert && (!certs[0].san || !certs[0].san_match))
                return false;
        if (!tls->check_cert)
                return true;

        if (!tls_date_now(address_of now) || !tls_leaf_authorized(certs, now))
                return false;
        for (i = 1; i < count; i++)
        {
                if (tls_spki_is_anchor(certs + i))
                        break;
                if (!tls_issuer_authorized(certs + i, i - 1, now))
                        return false;
        }

        for (i = 0; i < count; i++)
        {
                if (tls_spki_is_anchor(certs + i))
                        return i > 0;
                if (i + 1 < count)
                {
                        if (!tls_certificate_names_chain(certs + i,
                                                         certs + i + 1) ||
                            !tls_verify_one(certs + i, certs + i + 1))
                                return false;
                }
                else
                {
                        tls_cert root;

                        memory_fill(address_of root, 0, sizeof(root));
                        root.curve = 2;
                        memory_copy(root.qx, tls_isrg_x2_x, 48);
                        memory_copy(root.qy, tls_isrg_x2_y, 48);
                        if (tls_verify_one(certs + i, address_of root))
                                return true;
                        memory_copy(root.qx, tls_usertrust_ecc_x, 48);
                        memory_copy(root.qy, tls_usertrust_ecc_y, 48);
                        if (tls_verify_one(certs + i, address_of root))
                                return true;
                        root.curve = 3;
                        memory_copy(root.modulus, tls_isrg_x1_n, 512);
                        root.modulus_length = 512;
                        root.exponent = TLS_ISRG_X1_EXPONENT;
                        return tls_verify_one(certs + i, address_of root);
                }
        }

        return false;
}

static bool tls_hello_append(p8 address_to out, positive room,
                             positive address_to at,
                             const p8 address_to bytes, positive length)
{
        if (address_to at > room || length > room - address_to at)
                return false;

        memory_copy(out + address_to at, bytes, length);
        address_to at += length;
        return true;
}

static bipolar tls_client_hello(tls_conn address_to tls, p8 address_to out,
                                positive room, positive address_to used)
{
        static const p8 prefix[] = {
            TLS_HS_CLIENT_HELLO, 0, 0, 0, 0x03, 0x03};
        static const p8 parameters[] = {
            0,                    // empty legacy session id
            0, 2, 0x13, 0x01,    // TLS_AES_128_GCM_SHA256
            1, 0,                 // null legacy compression
            0, 0};                // extensions length, filled below
        static const p8 groups[] = {
            0, 0x0a, 0, 4, 0, 2, 0, 0x1d};
        static const p8 share[] = {
            0, 0x33, 0, 38, 0, 36, 0, 0x1d, 0, 32};
        static const p8 tail[] = {
            0, 0x2b, 0, 3, 2, 0x03, 0x04,
            0, 0x0d, 0, 8, 0, 6, 0x04, 0x03, 0x05, 0x03, 0x08, 0x04};
        p8 random[32];
        p8 public_key[32];
        p8 base[32];
        positive host_length = string_length(tls->host);
        bool named = string_to_host(tls->host) < 0;
        positive at = 0;
        positive ext_len_at;
        bipolar status = TLS_FAIL;

        if (system_random_fill(random, 32, 0) < 0)
                goto done;
        if (system_random_fill(tls->scalar, 32, 0) < 0)
                goto done;

        memory_fill(base, 0, 32);
        base[0] = 9;
        if (!crypto_x25519(public_key, tls->scalar, base))
                goto done;

        if (!tls_hello_append(out, room, address_of at, prefix, sizeof prefix) ||
            !tls_hello_append(out, room, address_of at, random, sizeof random) ||
            !tls_hello_append(out, room, address_of at, parameters,
                              sizeof parameters))
                goto done;

        ext_len_at = at - 2;

        if (named && host_length && host_length < 256)
        {
                positive n = 5 + host_length;
                p8 sni[] = {
                    0, 0, (p8)(n >> 8), (p8)n,
                    (p8)((n - 2) >> 8), (p8)(n - 2), 0,
                    (p8)(host_length >> 8), (p8)host_length};

                if (!tls_hello_append(out, room, address_of at, sni,
                                      sizeof sni) ||
                    !tls_hello_append(out, room, address_of at,
                                      (const p8 address_to)tls->host,
                                      host_length))
                        goto done;
        }

        if (!tls_hello_append(out, room, address_of at, groups, sizeof groups) ||
            !tls_hello_append(out, room, address_of at, share, sizeof share) ||
            !tls_hello_append(out, room, address_of at, public_key,
                              sizeof public_key) ||
            !tls_hello_append(out, room, address_of at, tail, sizeof tail))
                goto done;

        {
                positive ext_length = at - ext_len_at - 2;
                out[ext_len_at] = (p8)(ext_length >> 8);
                out[ext_len_at + 1] = (p8)ext_length;
        }
        {
                positive body = at - 4;
                out[1] = (p8)(body >> 16);
                out[2] = (p8)(body >> 8);
                out[3] = (p8)body;
        }

        address_to used = at;
        status = TLS_OK;

done:
        crypto_forget(random, sizeof random);
        crypto_forget(public_key, sizeof public_key);
        crypto_forget(base, sizeof base);
        if (status)
                crypto_forget(tls->scalar, sizeof tls->scalar);
        return status;
}

static bipolar tls_server_hello_share(p8 address_to hello, positive length,
                                      p8 address_to peer)
{
        positive at;
        positive ext_end;
        positive session;
        bool seen_share = false;
        bool seen_version = false;

        if (length < 44 || hello[0] != TLS_HS_SERVER_HELLO)
                return TLS_FAIL;
        {
                positive hs = ((positive)hello[1] << 16) | ((positive)hello[2] << 8) |
                              hello[3];
                if (hs + 4 != length)
                        return TLS_FAIL;
        }
        if (hello[4] != 0x03 || hello[5] != 0x03)
                return TLS_FAIL;

        at = 4 + 2 + 32;
        session = hello[at++];
        /* The client sent an empty legacy_session_id, so the echo is empty. */
        if (session)
                return TLS_FAIL;
        at += session;
        if (at + 3 > length)
                return TLS_FAIL;
        if (hello[at] != 0x13 || hello[at + 1] != 0x01)
                return TLS_FAIL;
        at += 2;
        if (hello[at++] != 0)
                return TLS_FAIL;
        if (at + 2 > length)
                return TLS_FAIL;
        {
                positive ext_length = ((positive)hello[at] << 8) | hello[at + 1];
                at += 2;
                ext_end = at + ext_length;
                if (ext_end != length)
                        return TLS_FAIL;
        }

        while (at < ext_end)
        {
                if (at + 4 > ext_end)
                        return TLS_FAIL;

                positive id = ((positive)hello[at] << 8) | hello[at + 1];
                positive elen = ((positive)hello[at + 2] << 8) | hello[at + 3];
                at += 4;
                if (at + elen > ext_end)
                        return TLS_FAIL;

                if (id == 0x002b)
                {
                        if (seen_version || elen != 2 || hello[at] != 0x03 ||
                            hello[at + 1] != 0x04)
                                return TLS_FAIL;
                        seen_version = true;
                }
                else if (id == 0x0033)
                {
                        if (seen_share || elen != 36 || hello[at] != 0 ||
                            hello[at + 1] != 0x1d ||
                            hello[at + 2] != 0 || hello[at + 3] != 32)
                                return TLS_FAIL;
                        memory_copy(peer, hello + at + 4, 32);
                        seen_share = true;
                }
                else
                        return TLS_FAIL;

                at += elen;
        }

        return seen_version && seen_share ? TLS_OK : TLS_FAIL;
}

#define TLS_HANDSHAKE_MORE 0
#define TLS_HANDSHAKE_COMPLETE 1

/* The handshake protocol is a byte stream layered over records.  ServerHello
   may therefore cross record boundaries, but it is the last plaintext
   handshake message: bytes after its declared end cannot legally share that
   plaintext stream. */
static bipolar tls_handshake_one_append(p8 address_to held, positive room,
                                        positive address_to held_length,
                                        p8 address_to fragment,
                                        positive length)
{
        positive body_length;
        positive complete;

        if (address_to held_length > room || !length ||
            length > room - address_to held_length)
                return TLS_FAIL;

        memory_copy(held + address_to held_length, fragment, length);
        address_to held_length += length;

        if (address_to held_length < 4)
                return TLS_HANDSHAKE_MORE;

        body_length = ((positive)held[1] << 16) |
                      ((positive)held[2] << 8) | held[3];
        if (body_length > room - 4)
                return TLS_FAIL;
        complete = 4 + body_length;
        if (address_to held_length < complete)
                return TLS_HANDSHAKE_MORE;

        return address_to held_length == complete ? TLS_HANDSHAKE_COMPLETE
                                                   : TLS_FAIL;
}

static bipolar tls_install_handshake_keys(tls_conn address_to tls, p8 address_to shared)
{
        p8 early[32];
        p8 zeros[32];
        p8 derived[32];
        p8 empty[32];

        memory_fill(zeros, 0, 32);
        tls_empty_hash(empty);
        crypto_hkdf_extract(zeros, 32, zeros, 32, early);
        tls_expand_label(early, "derived", empty, 32, derived, 32);
        crypto_hkdf_extract(derived, 32, shared, 32, tls->hs_secret);
        tls_derive_secret(tls->hs_secret, "c hs traffic",
                          address_of tls->transcript, tls->c_hs_traffic);
        tls_derive_secret(tls->hs_secret, "s hs traffic",
                          address_of tls->transcript, tls->s_hs_traffic);
        tls_traffic_keys(tls->c_hs_traffic, tls->c_key, tls->c_iv);
        tls_traffic_keys(tls->s_hs_traffic, tls->s_key, tls->s_iv);
        tls->seq_read = 0;
        tls->seq_write = 0;
        tls->encrypted = true;
        tls->application = false;

        crypto_forget(early, sizeof early);
        crypto_forget(zeros, sizeof zeros);
        crypto_forget(derived, sizeof derived);
        crypto_forget(empty, sizeof empty);
        return TLS_OK;
}

static fn tls_derive_app_keys(tls_conn address_to tls)
{
        p8 zeros[32];
        p8 derived[32];
        p8 empty[32];
        p8 master[32];

        memory_fill(zeros, 0, 32);
        tls_empty_hash(empty);
        tls_expand_label(tls->hs_secret, "derived", empty, 32, derived, 32);
        crypto_hkdf_extract(derived, 32, zeros, 32, master);
        tls_derive_secret(master, "c ap traffic", address_of tls->transcript,
                          tls->c_ap_traffic);
        tls_derive_secret(master, "s ap traffic", address_of tls->transcript,
                          tls->s_ap_traffic);

        crypto_forget(zeros, sizeof zeros);
        crypto_forget(derived, sizeof derived);
        crypto_forget(empty, sizeof empty);
        crypto_forget(master, sizeof master);
}

static fn tls_use_app_keys(tls_conn address_to tls)
{
        tls_traffic_keys(tls->c_ap_traffic, tls->c_key, tls->c_iv);
        tls_traffic_keys(tls->s_ap_traffic, tls->s_key, tls->s_iv);
        tls->seq_read = 0;
        tls->seq_write = 0;
        tls->application = true;

        crypto_forget(tls->hs_secret, sizeof tls->hs_secret);
        crypto_forget(tls->c_hs_traffic, sizeof tls->c_hs_traffic);
        crypto_forget(tls->s_hs_traffic, sizeof tls->s_hs_traffic);
        crypto_forget(tls->c_ap_traffic, sizeof tls->c_ap_traffic);
        crypto_forget(tls->s_ap_traffic, sizeof tls->s_ap_traffic);
        crypto_forget(address_of tls->transcript, sizeof tls->transcript);
}

static bipolar tls_check_finished(tls_conn address_to tls, p8 address_to verify,
                                  positive length)
{
        p8 finished_key[32];
        p8 expect[32];
        crypto_sha256 copy = tls->transcript;
        p8 hash[32];
        bipolar status = TLS_FAIL;

        if (length != 32)
                goto done;
        tls_expand_label(tls->s_hs_traffic, "finished", null, 0, finished_key, 32);
        crypto_sha256_close(address_of copy, hash);
        crypto_hmac_sha256(finished_key, 32, hash, 32, expect);
        status = memory_compare(expect, verify, 32) ? TLS_FAIL : TLS_OK;

done:
        crypto_forget(finished_key, sizeof finished_key);
        crypto_forget(expect, sizeof expect);
        crypto_forget(address_of copy, sizeof copy);
        crypto_forget(hash, sizeof hash);
        return status;
}

static bipolar tls_send_finished(tls_conn address_to tls)
{
        p8 finished_key[32];
        p8 verify[32];
        p8 msg[36];
        crypto_sha256 copy = tls->transcript;
        p8 hash[32];
        bipolar status = TLS_FAIL;

        tls_expand_label(tls->c_hs_traffic, "finished", null, 0, finished_key, 32);
        crypto_sha256_close(address_of copy, hash);
        crypto_hmac_sha256(finished_key, 32, hash, 32, verify);
        msg[0] = TLS_HS_FINISHED;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 32;
        memory_copy(msg + 4, verify, 32);
        if (tls_send_enc(tls, TLS_CT_HANDSHAKE, msg, 36))
                goto done;
        tls_transcript_add(tls, msg, 36);
        status = TLS_OK;

done:
        crypto_forget(finished_key, sizeof finished_key);
        crypto_forget(verify, sizeof verify);
        crypto_forget(msg, sizeof msg);
        crypto_forget(address_of copy, sizeof copy);
        crypto_forget(hash, sizeof hash);
        return status;
}

static bipolar tls_check_cert_verify(tls_conn address_to tls, p8 address_to msg,
                                     positive length)
{
        p8 signed_bytes[130];
        p8 hash[32];
        p8 r[48];
        p8 s[48];
        positive r_length = 0;
        positive s_length = 0;
        positive at;
        positive sig_length;
        p16 scheme;
        crypto_sha256 copy = tls->transcript;
        static const p8 context[] = "TLS 1.3, server CertificateVerify";

        if (length < 8)
                return TLS_FAIL;
        at = 4;
        scheme = (p16)((msg[at] << 8) | msg[at + 1]);
        at += 2;
        sig_length = ((positive)msg[at] << 8) | msg[at + 1];
        at += 2;
        if (at + sig_length != length)
                return TLS_FAIL;

        memory_fill(signed_bytes, 0x20, 64);
        memory_copy(signed_bytes + 64, context, 33);
        signed_bytes[97] = 0;
        crypto_sha256_close(address_of copy, hash);
        memory_copy(signed_bytes + 98, hash, 32);

        if (scheme == 0x0403)
        {
                crypto_sha256_of(signed_bytes, sizeof(signed_bytes), hash);
                if (tls_parse_ecdsa_sig(msg + at, sig_length, r, address_of r_length,
                                        s, address_of s_length))
                        return TLS_FAIL;
                if (tls->leaf_curve != 1)
                        return TLS_FAIL;
                return crypto_ecdsa_p256(hash, 32, r, r_length, s, s_length,
                                         tls->leaf_qx + 16, tls->leaf_qy + 16)
                           ? TLS_OK
                           : TLS_FAIL;
        }

        if (scheme == 0x0503)
        {
                p8 hash384[48];

                crypto_sha384(signed_bytes, sizeof(signed_bytes), hash384);
                if (tls_parse_ecdsa_sig(msg + at, sig_length, r, address_of r_length,
                                        s, address_of s_length))
                        return TLS_FAIL;
                if (tls->leaf_curve != 2)
                        return TLS_FAIL;
                return crypto_ecdsa_p384(hash384, 48, r, r_length, s, s_length,
                                         tls->leaf_qx, tls->leaf_qy)
                           ? TLS_OK
                           : TLS_FAIL;
        }

        if (scheme == 0x0804)
        {
                if (tls->leaf_curve != 3 || !tls->leaf_n_length)
                        return TLS_FAIL;
                return crypto_rsa_pss_sha256(tls->leaf_n, tls->leaf_n_length,
                                             tls->leaf_e, msg + at, sig_length,
                                             signed_bytes, sizeof(signed_bytes))
                           ? TLS_OK
                           : TLS_FAIL;
        }

        return TLS_FAIL;
}

#define TLS_SERVER_FLIGHT_EE 0
#define TLS_SERVER_FLIGHT_CERTIFICATE 1
#define TLS_SERVER_FLIGHT_CERT_VERIFY 2
#define TLS_SERVER_FLIGHT_FINISHED 3
#define TLS_SERVER_FLIGHT_COMPLETE 4

/* This client offers neither PSK nor client authentication, so the server
   flight has exactly one legal shape.  Keeping that shape in one transition
   function prevents a duplicate message from overwriting parsed certificate
   state or an early Finished from authenticating an incomplete transcript. */
static bool tls_server_flight_step(p8 address_to state, p8 type)
{
        static const p8 expected[] = {
            TLS_HS_ENCRYPTED_EXTS,
            TLS_HS_CERTIFICATE,
            TLS_HS_CERT_VERIFY,
            TLS_HS_FINISHED,
        };

        if (*state >= TLS_SERVER_FLIGHT_COMPLETE ||
            type != expected[*state])
                return false;

        (*state)++;
        return true;
}

static bool tls_encrypted_extensions_valid(p8 address_to body,
                                           positive length)
{
        positive at = 2;

        if (length < 2 || network_load_16(body) != length - 2)
                return false;

        while (at < length)
        {
                positive before = 2;
                p16 kind;
                p16 size;

                if (length - at < 4)
                        return false;

                kind = network_load_16(body + at);
                size = network_load_16(body + at + 2);

                if ((positive)size > length - at - 4)
                        return false;

                /* RFC 8446 forbids duplicate extensions.  A nested scan is
                   bounded by the 16 KiB handshake ceiling and avoids keeping
                   a second extension table solely for this check. */
                while (before < at)
                {
                        positive prior_size;

                        if (at - before < 4)
                                return false;
                        prior_size = network_load_16(body + before + 2);
                        if (prior_size > at - before - 4)
                                return false;
                        if (network_load_16(body + before) == kind)
                                return false;
                        before += 4 + prior_size;
                }

                at += 4 + size;
        }

        return at == length;
}

static bool tls_new_session_ticket_valid(p8 address_to body,
                                         positive length)
{
        positive at = 8;
        positive nonce_length;
        positive ticket_length;

        if (length < 13)
                return false;

        nonce_length = body[at++];
        if (nonce_length > length - at)
                return false;
        at += nonce_length;

        if (length - at < 2)
                return false;
        ticket_length = network_load_16(body + at);
        at += 2;
        if (!ticket_length || ticket_length > length - at)
                return false;
        at += ticket_length;

        return tls_encrypted_extensions_valid(body + at, length - at);
}

/* This client does not resume sessions, but servers commonly send tickets.
   Ignore only complete, well-framed NewSessionTicket messages.  KeyUpdate and
   every other unsupported post-handshake transition fail instead of leaving
   traffic keys or authentication state silently stale. */
static bipolar tls_post_handshake_append(p8 address_to held,
                                         positive address_to held_length,
                                         p8 address_to fragment,
                                         positive length)
{
        positive at = 0;

        if (*held_length > TLS_HS_MAX || !length ||
            length > TLS_HS_MAX - *held_length)
                return TLS_FAIL;

        memory_copy(held + *held_length, fragment, length);
        *held_length += length;

        while (at < *held_length)
        {
                positive body_length;

                if (*held_length - at < 4)
                        break;
                if (held[at] != TLS_HS_NEW_SESSION_TICKET)
                        return TLS_FAIL;

                body_length = ((positive)held[at + 1] << 16) |
                              ((positive)held[at + 2] << 8) |
                              held[at + 3];
                if (body_length > TLS_HS_MAX - 4)
                        return TLS_FAIL;

                if (body_length > *held_length - at - 4)
                        break;
                if (!tls_new_session_ticket_valid(held + at + 4,
                                                  body_length))
                        return TLS_FAIL;
                at += 4 + body_length;
        }

        if (at)
        {
                positive old_length = *held_length;

                memory_copy(held, held + at, *held_length - at);
                *held_length -= at;
                crypto_forget(held + *held_length,
                              old_length - *held_length);
        }

        return TLS_OK;
}

static bool tls_post_handshake_valid(p8 address_to messages,
                                     positive length)
{
        p8 held[TLS_HS_MAX];
        positive held_length = 0;
        bool valid;

        valid = tls_post_handshake_append(held, address_of held_length,
                                          messages, length) == TLS_OK &&
                !held_length;
        crypto_forget(held, sizeof held);
        return valid;
}

static bipolar tls_handshake(
    tls_conn address_to tls, const network_deadline address_to deadline)
{
        p8 hello[1024];
        p8 record[TLS_RECORD_MAX];
        p8 peer[32];
        p8 shared[32];
        positive hello_length = 0;
        p8 type = 0;
        positive length = 0;
        p8 hs[TLS_HS_MAX];
        positive hs_used = 0;
        p8 flight = TLS_SERVER_FLIGHT_EE;
        bool seen_ccs = false;
        bipolar status = TLS_FAIL;

        crypto_sha256_open(address_of tls->transcript);
        tls->leftover_used = 0;
        tls->encrypted = false;
        tls->application = false;

        if (tls_client_hello(tls, hello, sizeof(hello), address_of hello_length))
                goto done;
        tls_transcript_add(tls, hello, hello_length);
        if (tls_send_plain(tls, TLS_CT_HANDSHAKE, hello, hello_length))
                goto done;

        for (;;)
        {
                bipolar assembled;

                if (tls_read_record(tls, address_of type, record,
                                    sizeof(record), address_of length,
                                    deadline))
                        goto done;
                if (type == TLS_CT_CCS)
                {
                        if (!tls_compatibility_ccs_take(address_of seen_ccs))
                                goto done;
                        continue;
                }
                if (type != TLS_CT_HANDSHAKE)
                        goto done;

                assembled = tls_handshake_one_append(
                    hs, sizeof(hs), address_of hs_used, record, length);
                if (assembled == TLS_FAIL)
                        goto done;
                if (assembled == TLS_HANDSHAKE_COMPLETE)
                        break;
        }

        tls_transcript_add(tls, hs, hs_used);
        if (tls_server_hello_share(hs, hs_used, peer))
                goto done;
        hs_used = 0;

        if (!crypto_x25519(shared, tls->scalar, peer))
                goto done;
        crypto_forget(tls->scalar, sizeof tls->scalar);
        if (tls_install_handshake_keys(tls, shared))
                goto done;
        crypto_forget(shared, sizeof shared);

        while (flight != TLS_SERVER_FLIGHT_COMPLETE)
        {
                positive msg_at = 0;

                if (tls_read_record(tls, address_of type, record, sizeof(record),
                                    address_of length, deadline))
                        goto done;
                if (type == TLS_CT_CCS)
                {
                        if (!tls_compatibility_ccs_take(address_of seen_ccs))
                                goto done;
                        continue;
                }
                if (type != TLS_CT_HANDSHAKE)
                        goto done;
                if (hs_used + length > sizeof(hs))
                        goto done;
                memory_copy(hs + hs_used, record, length);
                hs_used += length;

                while (msg_at + 4 <= hs_used)
                {
                        p8 hs_type = hs[msg_at];
                        positive hs_len = ((positive)hs[msg_at + 1] << 16) |
                                          ((positive)hs[msg_at + 2] << 8) |
                                          hs[msg_at + 3];
                        if (msg_at + 4 + hs_len > hs_used)
                                break;

                        if (!tls_server_flight_step(address_of flight,
                                                    hs_type))
                                goto done;

                        if (hs_type == TLS_HS_ENCRYPTED_EXTS)
                        {
                                if (!tls_encrypted_extensions_valid(
                                        hs + msg_at + 4, hs_len))
                                        goto done;
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                        }
                        else if (hs_type == TLS_HS_CERTIFICATE)
                        {
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                                if (!tls_verify_chain(hs + msg_at + 4, hs_len, tls->host,
                                                      tls))
                                        goto done;
                        }
                        else if (hs_type == TLS_HS_CERT_VERIFY)
                        {
                                if (tls_check_cert_verify(tls, hs + msg_at, 4 + hs_len))
                                        goto done;
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                        }
                        else if (hs_type == TLS_HS_FINISHED)
                        {
                                if (tls_check_finished(tls, hs + msg_at + 4, hs_len))
                                        goto done;
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                        }

                        msg_at += 4 + hs_len;
                }

                if (msg_at)
                {
                        memory_copy(hs, hs + msg_at, hs_used - msg_at);
                        hs_used -= msg_at;
                }

                if (flight == TLS_SERVER_FLIGHT_COMPLETE && hs_used)
                        goto done;
        }

        tls_derive_app_keys(tls);
        if (tls_send_finished(tls))
                goto done;
        tls_use_app_keys(tls);
        status = TLS_OK;

done:
        crypto_forget(hello, sizeof hello);
        crypto_forget(record, sizeof record);
        crypto_forget(peer, sizeof peer);
        crypto_forget(shared, sizeof shared);
        crypto_forget(hs, sizeof hs);
        crypto_forget(tls->scalar, sizeof tls->scalar);
        return status;
}

static bipolar tls_connect(tls_conn address_to tls, bipolar handle,
                           string_address host, bool check_cert)
{
        bipolar status;
        network_deadline deadline;

        memory_fill(tls, 0, sizeof(*tls));
        tls->handle = handle;
        tls->host = host;
        tls->check_cert = check_cert;
        status = network_deadline_begin(address_of deadline,
                                        TLS_HANDSHAKE_SECONDS, 0)
                     ? tls_handshake(tls, address_of deadline) : TLS_FAIL;
        if (status)
                tls_forget(tls);
        return status;
}

static bipolar tls_write(tls_conn address_to tls, p8 address_to data,
                         positive length)
{
        while (length)
        {
                positive take = length;

                if (take > 16384)
                        take = 16384;
                if (tls_send_enc(tls, TLS_CT_APP, data, take))
                        return TLS_FAIL;
                data += take;
                length -= take;
        }

        return TLS_OK;
}

static bipolar tls_read_until(
    tls_conn address_to tls, p8 address_to into, positive room,
    positive address_to got, const network_deadline address_to deadline)
{
        p8 type = 0;
        p8 record[TLS_RECORD_MAX];
        positive length = 0;
        p8 post_handshake[TLS_HS_MAX];
        positive post_handshake_used = 0;
        bipolar status;
        bipolar result = TLS_FAIL;

        if (tls->leftover_used)
        {
                positive old_used = tls->leftover_used;
                positive take = tls->leftover_used;

                if (take > room)
                        take = room;
                memory_copy(into, tls->leftover, take);
                memory_copy(tls->leftover, tls->leftover + take,
                            tls->leftover_used - take);
                tls->leftover_used -= take;
                crypto_forget(tls->leftover + tls->leftover_used,
                              old_used - tls->leftover_used);
                address_to got = take;
                return TLS_OK;
        }

        for (;;)
        {
                status = tls_read_record(tls, address_of type, record, sizeof(record),
                                         address_of length, deadline);
                if (status == TLS_EOF)
                {
                        if (post_handshake_used)
                                goto done;
                        address_to got = 0;
                        result = TLS_OK;
                        goto done;
                }
                if (status)
                        goto done;
                if (type == TLS_CT_CCS)
                        goto done;
                if (type == TLS_CT_HANDSHAKE)
                {
                        if (tls_post_handshake_append(
                                post_handshake,
                                address_of post_handshake_used,
                                record, length))
                                goto done;
                        crypto_forget(record, length);
                        continue;
                }
                if (type != TLS_CT_APP)
                        goto done;
                if (post_handshake_used)
                        goto done;
                if (!length)
                        continue;
                if (length <= room)
                {
                        memory_copy(into, record, length);
                        address_to got = length;
                        result = TLS_OK;
                        goto done;
                }
                memory_copy(into, record, room);
                memory_copy(tls->leftover, record + room, length - room);
                tls->leftover_used = length - room;
                address_to got = room;
                result = TLS_OK;
                goto done;
        }

done:
        crypto_forget(record, sizeof record);
        crypto_forget(post_handshake, sizeof post_handshake);
        return result;
}

static bipolar tls_read(tls_conn address_to tls, p8 address_to into,
                        positive room, positive address_to got)
{
        return tls_read_until(tls, into, room, got, null);
}

#endif
