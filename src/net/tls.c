/*
        TLS 1.3 client for wget.

        One cipher: TLS_AES_128_GCM_SHA256. One group: X25519. Certificates
        walk to ISRG Root X2 (ECDSA P-384), which is the Let's Encrypt root
        the Arch bootstrap mirror presents. Signature algorithms advertised
        are ecdsa_secp256r1_sha256 and ecdsa_secp384r1_sha384 only; a PSS
        leaf is not something this client can verify. close_notify is a
        clean end of the body, not a handshake failure.
        --no-check-certificate skips the chain and still encrypts.
*/

#ifndef STANDARD_MODERN_C_NET_TLS
#define STANDARD_MODERN_C_NET_TLS

#include "crypto.c"

#define TLS_RECORD_MAX 16640
#define TLS_HS_MAX 16384
#define TLS_OK 0
#define TLS_FAIL (-1)
#define TLS_EOF 1

#define TLS_CT_CCS 20
#define TLS_CT_ALERT 21
#define TLS_CT_HANDSHAKE 22
#define TLS_CT_APP 23

#define TLS_HS_CLIENT_HELLO 1
#define TLS_HS_SERVER_HELLO 2
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
        p8 leaf_curve;
} tls_conn;

static bipolar tls_read_full(bipolar handle, p8 address_to into, positive want)
{
        positive have = 0;

        if (!want)
                return TLS_OK;

        while (have < want)
        {
                bipolar got = system_read_retry((positive)handle, into + have,
                                                want - have);
                if (got < 0)
                        return TLS_FAIL;
                if (!got)
                        return have ? TLS_FAIL : TLS_EOF;
                have += (positive)got;
        }

        return TLS_OK;
}

static bipolar tls_write_full(bipolar handle, p8 address_to data, positive length)
{
        if (system_write_all((positive)handle, data, length) != length)
                return TLS_FAIL;
        return TLS_OK;
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
}

static fn tls_derive_secret(p8 address_to secret, string_address label,
                            crypto_sha256 address_to transcript,
                            p8 address_to out)
{
        crypto_sha256 copy = *transcript;
        p8 hash[32];

        crypto_sha256_close(address_of copy, hash);
        tls_expand_label(secret, label, hash, 32, out, 32);
}

static fn tls_empty_hash(p8 address_to out)
{
        crypto_sha256 hash;

        crypto_sha256_open(address_of hash);
        crypto_sha256_close(address_of hash, out);
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
        if (tls_write_full(tls->handle, header, 5))
                return TLS_FAIL;
        return tls_write_full(tls->handle, body, length);
}

static bipolar tls_send_enc(tls_conn address_to tls, p8 inner_type,
                            p8 address_to body, positive length)
{
        p8 inner[TLS_RECORD_MAX];
        p8 header[5];
        p8 nonce[12];
        p8 tag[16];
        p8 aad[5];
        positive inner_length = length + 1;
        positive record = inner_length + 16;

        if (record > TLS_RECORD_MAX)
                return TLS_FAIL;

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

        if (tls_write_full(tls->handle, header, 5))
                return TLS_FAIL;
        if (tls_write_full(tls->handle, inner, inner_length))
                return TLS_FAIL;
        return tls_write_full(tls->handle, tag, 16);
}

static bipolar tls_decrypt_record(tls_conn address_to tls, p8 address_to payload,
                                  positive payload_length, p8 address_to aad,
                                  p8 address_to inner, positive address_to inner_length,
                                  p8 address_to type)
{
        p8 nonce[12];
        p8 tag[16];
        positive at;

        if (payload_length < 16)
                return TLS_FAIL;

        memory_copy(inner, payload, payload_length - 16);
        memory_copy(tag, payload + payload_length - 16, 16);
        tls_nonce(tls->s_iv, tls->seq_read, nonce);
        if (!crypto_aesgcm_decrypt(tls->s_key, nonce, aad, 5, inner,
                                   payload_length - 16, tag))
                return TLS_FAIL;

        tls->seq_read++;
        at = payload_length - 16;
        while (at && inner[at - 1] == 0)
                at--;
        if (!at)
                return TLS_FAIL;
        address_to type = inner[at - 1];
        address_to inner_length = at - 1;
        return TLS_OK;
}

static bipolar tls_read_record(tls_conn address_to tls, p8 address_to type,
                               p8 address_to body, positive room,
                               positive address_to length)
{
        p8 header[5];
        p8 payload[TLS_RECORD_MAX];
        positive payload_length;
        p8 inner[TLS_RECORD_MAX];
        positive inner_length = 0;
        p8 inner_type = 0;
        bipolar status;

        status = tls_read_full(tls->handle, header, 5);
        if (status)
                return status;

        payload_length = ((positive)header[3] << 8) | header[4];
        if (!payload_length || payload_length > TLS_RECORD_MAX)
                return TLS_FAIL;
        if (tls_read_full(tls->handle, payload, payload_length))
                return TLS_FAIL;

        if (header[0] == TLS_CT_CCS)
        {
                address_to type = TLS_CT_CCS;
                address_to length = 0;
                return TLS_OK;
        }

        if (header[0] == TLS_CT_ALERT)
                return (payload_length >= 2 && payload[1] == 0) ? TLS_EOF
                                                                : TLS_FAIL;

        if (!tls->encrypted)
        {
                if (header[0] != TLS_CT_HANDSHAKE || payload_length > room)
                        return TLS_FAIL;
                memory_copy(body, payload, payload_length);
                address_to type = TLS_CT_HANDSHAKE;
                address_to length = payload_length;
                return TLS_OK;
        }

        if (tls_decrypt_record(tls, payload, payload_length, header, inner,
                               address_of inner_length, address_of inner_type))
                return TLS_FAIL;

        if (inner_type == TLS_CT_ALERT)
                return (inner_length >= 2 && inner[1] == 0) ? TLS_EOF : TLS_FAIL;

        if (inner_length > room)
                return TLS_FAIL;
        memory_copy(body, inner, inner_length);
        address_to type = inner_type;
        address_to length = inner_length;
        return TLS_OK;
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
        if (!count || count > 3 || i + count > size)
                return TLS_FAIL;
        while (count)
        {
                value = (value << 8) | bytes[i++];
                count--;
        }
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

static bool tls_name_in_san(p8 address_to cert, positive length,
                            string_address host)
{
        positive at = 0;
        positive stop = 0;
        positive tbs_stop = 0;

        if (tls_asn1_enter(cert, length, 0x30, address_of at, address_of stop))
                return false;
        if (tls_asn1_enter(cert, length, 0x30, address_of at, address_of tbs_stop))
                return false;

        if (at < tbs_stop && cert[at] == 0xa0 &&
            tls_asn1_skip(cert, tbs_stop, address_of at))
                return false;
        if (tls_asn1_skip(cert, tbs_stop, address_of at) ||
            tls_asn1_skip(cert, tbs_stop, address_of at) ||
            tls_asn1_skip(cert, tbs_stop, address_of at) ||
            tls_asn1_skip(cert, tbs_stop, address_of at) ||
            tls_asn1_skip(cert, tbs_stop, address_of at) ||
            tls_asn1_skip(cert, tbs_stop, address_of at))
                return false;

        while (at < tbs_stop)
        {
                positive ext_stop = 0;
                positive seq_stop = 0;

                if (cert[at] != 0xa3)
                {
                        if (tls_asn1_skip(cert, tbs_stop, address_of at))
                                return false;
                        continue;
                }

                if (tls_asn1_enter(cert, tbs_stop, 0xa3, address_of at,
                                   address_of ext_stop))
                        return false;
                if (tls_asn1_enter(cert, ext_stop, 0x30, address_of at,
                                   address_of seq_stop))
                        return false;

                while (at < seq_stop)
                {
                        positive one_stop = 0;
                        positive oid_stop = 0;
                        positive one_at;

                        if (tls_asn1_enter(cert, seq_stop, 0x30, address_of at,
                                           address_of one_stop))
                                return false;
                        one_at = at;
                        if (tls_asn1_enter(cert, one_stop, 0x06, address_of one_at,
                                           address_of oid_stop))
                                return false;
                        if (tls_oid_is(cert + one_at, oid_stop - one_at, tls_oid_san,
                                       3))
                        {
                                one_at = oid_stop;
                                if (one_at < one_stop && cert[one_at] == 0x01 &&
                                    tls_asn1_skip(cert, one_stop, address_of one_at))
                                        return false;
                                {
                                        positive san_stop = 0;
                                        positive list_stop = 0;

                                        if (tls_asn1_enter(cert, one_stop, 0x04,
                                                           address_of one_at,
                                                           address_of san_stop))
                                                return false;
                                        if (tls_asn1_enter(cert, san_stop, 0x30,
                                                           address_of one_at,
                                                           address_of list_stop))
                                                return false;
                                        while (one_at < list_stop)
                                        {
                                                positive nstop = 0;
                                                p8 tag = cert[one_at];

                                                if (tls_asn1_enter(cert, list_stop, tag,
                                                                   address_of one_at,
                                                                   address_of nstop))
                                                        return false;
                                                if ((tag & 0x1f) == 2 &&
                                                    tls_host_match(host,
                                                                   cert + one_at,
                                                                   nstop - one_at))
                                                        return true;
                                                one_at = nstop;
                                        }
                                }
                        }
                        at = one_stop;
                }
                return false;
        }

        return false;
}

static bipolar tls_parse_ecdsa_sig(p8 address_to sig, positive length,
                                   p8 address_to r, positive address_to r_length,
                                   p8 address_to s, positive address_to s_length)
{
        positive at = 0;
        positive stop = 0;
        positive r_stop = 0;
        positive s_stop = 0;

        if (tls_asn1_enter(sig, length, 0x30, address_of at, address_of stop))
                return TLS_FAIL;
        if (tls_asn1_enter(sig, stop, 0x02, address_of at, address_of r_stop))
                return TLS_FAIL;
        while (at < r_stop && sig[at] == 0)
                at++;
        address_to r_length = r_stop - at;
        if (!address_to r_length || address_to r_length > 48)
                return TLS_FAIL;
        memory_copy(r, sig + at, address_to r_length);
        at = r_stop;
        if (tls_asn1_enter(sig, stop, 0x02, address_of at, address_of s_stop))
                return TLS_FAIL;
        while (at < s_stop && sig[at] == 0)
                at++;
        address_to s_length = s_stop - at;
        if (!address_to s_length || address_to s_length > 48)
                return TLS_FAIL;
        memory_copy(s, sig + at, address_to s_length);
        return TLS_OK;
}

typedef struct
{
        p8 address_to tbs;
        positive tbs_length;
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
} tls_cert;

static bipolar tls_parse_cert(p8 address_to der, positive length, tls_cert address_to cert)
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

        if (tls_asn1_enter(der, length, 0x30, address_of at, address_of stop))
                return TLS_FAIL;
        cert->tbs = der + at;
        if (tls_asn1_enter(der, length, 0x30, address_of at, address_of tbs_stop))
                return TLS_FAIL;
        cert->tbs_length = (positive)((der + tbs_stop) - cert->tbs);

        at = tbs_stop;
        if (tls_asn1_enter(der, stop, 0x30, address_of at, address_of alg_stop))
                return TLS_FAIL;
        {
                positive oid_at = at;
                if (tls_asn1_enter(der, alg_stop, 0x06, address_of oid_at,
                                   address_of oid_stop))
                        return TLS_FAIL;
                cert->sig_oid = der + oid_at;
                cert->sig_oid_length = oid_stop - oid_at;
        }
        at = alg_stop;
        if (tls_asn1_enter(der, stop, 0x03, address_of at, address_of bit_stop))
                return TLS_FAIL;
        if (at >= bit_stop)
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
        if (tls_asn1_skip(der, tbs_stop, address_of at) ||
            tls_asn1_skip(der, tbs_stop, address_of at) ||
            tls_asn1_skip(der, tbs_stop, address_of at) ||
            tls_asn1_skip(der, tbs_stop, address_of at) ||
            tls_asn1_skip(der, tbs_stop, address_of at))
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
                                   address_of curve_stop))
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
                                   address_of bit_stop))
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0)
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0x04)
                        return TLS_FAIL;
                coord = cert->curve == 1 ? 32 : 48;
                if (at + coord * 2 > bit_stop)
                        return TLS_FAIL;
                memory_copy(cert->qx + (48 - coord), der + at, coord);
                memory_copy(cert->qy + (48 - coord), der + at + coord, coord);
        }
        else if (tls_oid_is(der + param_at, oid_stop - param_at, tls_oid_rsa, 9))
        {
                positive n_stop = 0;
                positive e_stop = 0;
                positive rsa_stop = 0;

                cert->curve = 3;
                at = alg_stop;
                if (tls_asn1_enter(der, spki_stop, 0x03, address_of at,
                                   address_of bit_stop))
                        return TLS_FAIL;
                if (at >= bit_stop || der[at++] != 0)
                        return TLS_FAIL;
                if (tls_asn1_enter(der, bit_stop, 0x30, address_of at,
                                   address_of rsa_stop))
                        return TLS_FAIL;
                if (tls_asn1_enter(der, rsa_stop, 0x02, address_of at,
                                   address_of n_stop))
                        return TLS_FAIL;
                while (at < n_stop && der[at] == 0)
                        at++;
                cert->modulus_length = n_stop - at;
                if (cert->modulus_length > sizeof(cert->modulus))
                        return TLS_FAIL;
                memory_copy(cert->modulus, der + at, cert->modulus_length);
                at = n_stop;
                if (tls_asn1_enter(der, rsa_stop, 0x02, address_of at,
                                   address_of e_stop))
                        return TLS_FAIL;
                cert->exponent = 0;
                while (at < e_stop)
                        cert->exponent = (cert->exponent << 8) | der[at++];
        }
        else
                return TLS_FAIL;

        return TLS_OK;
}

static bool tls_spki_is_x2(tls_cert address_to cert)
{
        return cert->curve == 2 &&
               !memory_compare(cert->qx, tls_isrg_x2_x, 48) &&
               !memory_compare(cert->qy, tls_isrg_x2_y, 48);
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
        memory_copy(tls->leaf_qx, leaf->qx, 48);
        memory_copy(tls->leaf_qy, leaf->qy, 48);
}

static bool tls_verify_chain(p8 address_to body, positive body_length,
                             string_address host, tls_conn address_to tls)
{
        tls_cert certs[8];
        positive count = 0;
        positive at;
        positive list_end;
        positive i;
        p8 address_to leaf_der = null;
        positive leaf_length = 0;

        if (body_length < 4)
                return false;
        at = 1 + body[0];
        if (at + 3 > body_length)
                return false;
        {
                positive list_length = ((positive)body[at] << 16) |
                                       ((positive)body[at + 1] << 8) | body[at + 2];
                at += 3;
                if (at + list_length > body_length)
                        return false;
                list_end = at + list_length;
        }

        while (at + 3 <= list_end && count < 8)
        {
                positive cert_length = ((positive)body[at] << 16) |
                                       ((positive)body[at + 1] << 8) | body[at + 2];
                positive ext_length;

                at += 3;
                if (at + cert_length + 2 > list_end)
                        return false;
                if (tls_parse_cert(body + at, cert_length, certs + count))
                        return false;
                if (!count)
                {
                        leaf_der = body + at;
                        leaf_length = cert_length;
                }
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

        if (tls->check_cert && !tls_name_in_san(leaf_der, leaf_length, host))
                return false;
        if (!tls->check_cert)
                return true;

        for (i = 0; i < count; i++)
        {
                if (tls_spki_is_x2(certs + i))
                        return i > 0;
                if (i + 1 < count)
                {
                        if (!tls_verify_one(certs + i, certs + i + 1))
                                return false;
                }
                else
                {
                        tls_cert root;

                        memory_fill(address_of root, 0, sizeof(root));
                        root.curve = 2;
                        memory_copy(root.qx, tls_isrg_x2_x, 48);
                        memory_copy(root.qy, tls_isrg_x2_y, 48);
                        return tls_verify_one(certs + i, address_of root);
                }
        }

        return false;
}

static bipolar tls_client_hello(tls_conn address_to tls, p8 address_to out,
                                positive room, positive address_to used)
{
        p8 random[32];
        p8 public_key[32];
        p8 base[32];
        positive host_length = string_length(tls->host);
        bool named = string_to_host(tls->host) < 0;
        positive at = 0;
        positive ext_len_at;

        if (system_call_3(syscall(getrandom), (positive)random, 32, 0) != 32)
                return TLS_FAIL;
        if (system_call_3(syscall(getrandom), (positive)tls->scalar, 32, 0) != 32)
                return TLS_FAIL;

        memory_fill(base, 0, 32);
        base[0] = 9;
        crypto_x25519(public_key, tls->scalar, base);

        out[at++] = TLS_HS_CLIENT_HELLO;
        out[at++] = 0;
        out[at++] = 0;
        out[at++] = 0;
        out[at++] = 0x03;
        out[at++] = 0x03;
        memory_copy(out + at, random, 32);
        at += 32;
        out[at++] = 0;
        out[at++] = 0;
        out[at++] = 2;
        out[at++] = 0x13;
        out[at++] = 0x01;
        out[at++] = 1;
        out[at++] = 0;

        ext_len_at = at;
        at += 2;

        if (named && host_length && host_length < 256)
        {
                positive n = 5 + host_length;

                out[at++] = 0;
                out[at++] = 0;
                out[at++] = (p8)(n >> 8);
                out[at++] = (p8)n;
                n -= 2;
                out[at++] = (p8)(n >> 8);
                out[at++] = (p8)n;
                out[at++] = 0;
                out[at++] = (p8)(host_length >> 8);
                out[at++] = (p8)host_length;
                memory_copy(out + at, tls->host, host_length);
                at += host_length;
        }

        out[at++] = 0;
        out[at++] = 0x0a;
        out[at++] = 0;
        out[at++] = 4;
        out[at++] = 0;
        out[at++] = 2;
        out[at++] = 0;
        out[at++] = 0x1d;

        out[at++] = 0;
        out[at++] = 0x33;
        out[at++] = 0;
        out[at++] = 38;
        out[at++] = 0;
        out[at++] = 36;
        out[at++] = 0;
        out[at++] = 0x1d;
        out[at++] = 0;
        out[at++] = 32;
        memory_copy(out + at, public_key, 32);
        at += 32;

        out[at++] = 0;
        out[at++] = 0x2b;
        out[at++] = 0;
        out[at++] = 3;
        out[at++] = 2;
        out[at++] = 0x03;
        out[at++] = 0x04;

        out[at++] = 0;
        out[at++] = 0x0d;
        out[at++] = 0;
        out[at++] = 6;
        out[at++] = 0;
        out[at++] = 4;
        out[at++] = 0x04;
        out[at++] = 0x03;
        out[at++] = 0x05;
        out[at++] = 0x03;

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

        if (at > room)
                return TLS_FAIL;
        address_to used = at;
        return TLS_OK;
}

static bipolar tls_server_hello_share(p8 address_to hello, positive length,
                                      p8 address_to peer)
{
        positive at;
        positive ext_end;
        positive session;

        if (length < 38 || hello[0] != TLS_HS_SERVER_HELLO)
                return TLS_FAIL;
        {
                positive hs = ((positive)hello[1] << 16) | ((positive)hello[2] << 8) |
                              hello[3];
                if (hs + 4 != length)
                        return TLS_FAIL;
        }
        at = 4 + 2 + 32;
        session = hello[at++];
        at += session;
        if (at + 3 > length)
                return TLS_FAIL;
        if (hello[at] != 0x13 || hello[at + 1] != 0x01)
                return TLS_FAIL;
        at += 3;
        if (at + 2 > length)
                return TLS_FAIL;
        {
                positive ext_length = ((positive)hello[at] << 8) | hello[at + 1];
                at += 2;
                ext_end = at + ext_length;
                if (ext_end != length)
                        return TLS_FAIL;
        }

        while (at + 4 <= ext_end)
        {
                positive id = ((positive)hello[at] << 8) | hello[at + 1];
                positive elen = ((positive)hello[at + 2] << 8) | hello[at + 3];
                at += 4;
                if (at + elen > ext_end)
                        return TLS_FAIL;
                if (id == 0x0033)
                {
                        if (elen < 36 || hello[at] != 0 || hello[at + 1] != 0x1d ||
                            hello[at + 2] != 0 || hello[at + 3] != 32)
                                return TLS_FAIL;
                        memory_copy(peer, hello + at + 4, 32);
                        return TLS_OK;
                }
                at += elen;
        }

        return TLS_FAIL;
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
}

static fn tls_use_app_keys(tls_conn address_to tls)
{
        tls_traffic_keys(tls->c_ap_traffic, tls->c_key, tls->c_iv);
        tls_traffic_keys(tls->s_ap_traffic, tls->s_key, tls->s_iv);
        tls->seq_read = 0;
        tls->seq_write = 0;
        tls->application = true;
}

static bipolar tls_check_finished(tls_conn address_to tls, p8 address_to verify,
                                  positive length)
{
        p8 finished_key[32];
        p8 expect[32];
        crypto_sha256 copy = tls->transcript;
        p8 hash[32];

        if (length != 32)
                return TLS_FAIL;
        tls_expand_label(tls->s_hs_traffic, "finished", null, 0, finished_key, 32);
        crypto_sha256_close(address_of copy, hash);
        crypto_hmac_sha256(finished_key, 32, hash, 32, expect);
        return memory_compare(expect, verify, 32) ? TLS_FAIL : TLS_OK;
}

static bipolar tls_send_finished(tls_conn address_to tls)
{
        p8 finished_key[32];
        p8 verify[32];
        p8 msg[36];
        crypto_sha256 copy = tls->transcript;
        p8 hash[32];

        tls_expand_label(tls->c_hs_traffic, "finished", null, 0, finished_key, 32);
        crypto_sha256_close(address_of copy, hash);
        crypto_hmac_sha256(finished_key, 32, hash, 32, verify);
        msg[0] = TLS_HS_FINISHED;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 32;
        memory_copy(msg + 4, verify, 32);
        if (tls_send_enc(tls, TLS_CT_HANDSHAKE, msg, 36))
                return TLS_FAIL;
        tls_transcript_add(tls, msg, 36);
        return TLS_OK;
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

        return TLS_FAIL;
}

static bipolar tls_handshake(tls_conn address_to tls)
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
        bool seen_ee = false;
        bool seen_cert = false;
        bool seen_cv = false;
        bool seen_fin = false;

        crypto_sha256_open(address_of tls->transcript);
        tls->leftover_used = 0;
        tls->encrypted = false;
        tls->application = false;

        if (tls_client_hello(tls, hello, sizeof(hello), address_of hello_length))
                return TLS_FAIL;
        tls_transcript_add(tls, hello, hello_length);
        if (tls_send_plain(tls, TLS_CT_HANDSHAKE, hello, hello_length))
                return TLS_FAIL;

        if (tls_read_record(tls, address_of type, record, sizeof(record),
                            address_of length) ||
            type != TLS_CT_HANDSHAKE)
                return TLS_FAIL;
        tls_transcript_add(tls, record, length);
        if (tls_server_hello_share(record, length, peer))
                return TLS_FAIL;

        crypto_x25519(shared, tls->scalar, peer);
        if (tls_install_handshake_keys(tls, shared))
                return TLS_FAIL;

        while (!seen_fin)
        {
                positive msg_at = 0;

                if (tls_read_record(tls, address_of type, record, sizeof(record),
                                    address_of length))
                        return TLS_FAIL;
                if (type == TLS_CT_CCS)
                        continue;
                if (type != TLS_CT_HANDSHAKE)
                        return TLS_FAIL;
                if (hs_used + length > sizeof(hs))
                        return TLS_FAIL;
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

                        if (hs_type == TLS_HS_ENCRYPTED_EXTS)
                        {
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                                seen_ee = true;
                        }
                        else if (hs_type == TLS_HS_CERTIFICATE)
                        {
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                                if (!tls_verify_chain(hs + msg_at + 4, hs_len, tls->host,
                                                      tls))
                                        return TLS_FAIL;
                                seen_cert = true;
                        }
                        else if (hs_type == TLS_HS_CERT_VERIFY)
                        {
                                if (tls_check_cert_verify(tls, hs + msg_at, 4 + hs_len))
                                        return TLS_FAIL;
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                                seen_cv = true;
                        }
                        else if (hs_type == TLS_HS_FINISHED)
                        {
                                if (tls_check_finished(tls, hs + msg_at + 4, hs_len))
                                        return TLS_FAIL;
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);
                                seen_fin = true;
                        }
                        else
                                tls_transcript_add(tls, hs + msg_at, 4 + hs_len);

                        msg_at += 4 + hs_len;
                }

                if (msg_at)
                {
                        memory_copy(hs, hs + msg_at, hs_used - msg_at);
                        hs_used -= msg_at;
                }
        }

        if (!seen_ee || !seen_cert || !seen_cv)
                return TLS_FAIL;
        tls_derive_app_keys(tls);
        if (tls_send_finished(tls))
                return TLS_FAIL;
        tls_use_app_keys(tls);
        return TLS_OK;
}

static bipolar tls_connect(tls_conn address_to tls, bipolar handle,
                           string_address host, bool check_cert)
{
        memory_fill(tls, 0, sizeof(*tls));
        tls->handle = handle;
        tls->host = host;
        tls->check_cert = check_cert;
        return tls_handshake(tls);
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

static bipolar tls_read(tls_conn address_to tls, p8 address_to into, positive room,
                        positive address_to got)
{
        p8 type = 0;
        p8 record[TLS_RECORD_MAX];
        positive length = 0;
        bipolar status;

        if (tls->leftover_used)
        {
                positive take = tls->leftover_used;
                if (take > room)
                        take = room;
                memory_copy(into, tls->leftover, take);
                memory_copy(tls->leftover, tls->leftover + take,
                            tls->leftover_used - take);
                tls->leftover_used -= take;
                address_to got = take;
                return TLS_OK;
        }

        for (;;)
        {
                status = tls_read_record(tls, address_of type, record, sizeof(record),
                                         address_of length);
                if (status == TLS_EOF)
                {
                        address_to got = 0;
                        return TLS_OK;
                }
                if (status)
                        return TLS_FAIL;
                if (type == TLS_CT_CCS || type == TLS_CT_HANDSHAKE)
                        continue;
                if (type != TLS_CT_APP)
                        return TLS_FAIL;
                if (length <= room)
                {
                        memory_copy(into, record, length);
                        address_to got = length;
                        return TLS_OK;
                }
                memory_copy(into, record, room);
                memory_copy(tls->leftover, record + room, length - room);
                tls->leftover_used = length - room;
                address_to got = room;
                return TLS_OK;
        }
}

#endif
