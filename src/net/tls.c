/*
        TLS 1.3 client for wget.

        One cipher: TLS_AES_128_GCM_SHA256. Groups: X25519, P-256 and
        P-384, each with a ClientHello key share so Chimera's secp384r1
        servers do not HelloRetryRequest. Certificates walk to one of the
        Mozilla TLS roots in anchors.inc: a served certificate carrying an
        anchor's key ends the chain, or the last one served names an anchor
        as its issuer and verifies under it. Chain signatures may be ECDSA
        with SHA-256 or SHA-384 on P-256 or P-384, or RSA PKCS#1 v1.5 with
        SHA-256 or SHA-384.
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
/* One receive takes as many whole records as the socket has queued and this
   room holds: about fifteen full records. At least two whole records must
   fit, since the unopened tail moves to the front only when a record would
   not. */
#ifndef TLS_RECEIVE_ROOM
#define TLS_RECEIVE_ROOM ((positive)1 << 18)
#endif
#define TLS_HS_MAX 16384
#define TLS_HANDSHAKE_SECONDS 30
#define TLS_OK 0
#define TLS_FAIL (-1)
#define TLS_EOF 1
#define TLS_AGAIN 2

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

/* One trust anchor from anchors.inc: hashes that find candidates, then the
   key itself (curve 1 P-256, 2 P-384, 3 RSA). */
typedef struct
{
        p8 name[8];
        p8 key_hash[8];
        p8 curve;
        p32 exponent;
        p16 key_length;
        string_address key;
} tls_anchor;

static const tls_anchor tls_anchors[] = {
#include "anchors.inc"
};

static const p8 tls_oid_ec[7] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const p8 tls_oid_p256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const p8 tls_oid_p384[5] = {0x2b, 0x81, 0x04, 0x00, 0x22};
static const p8 tls_oid_ecdsa_sha256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04,
                                           0x03, 0x02};
static const p8 tls_oid_ecdsa_sha384[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04,
                                           0x03, 0x03};
static const p8 tls_oid_sha256_rsa[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
                                         0x01, 0x01, 0x0b};
static const p8 tls_oid_sha384_rsa[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
                                         0x01, 0x01, 0x0c};
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
        p8 x25519_scalar[32];
        p8 p256_scalar[32];
        p8 p384_scalar[48];
        p8 hs_secret[32];
        p8 c_hs_traffic[32];
        p8 s_hs_traffic[32];
        p8 c_ap_traffic[32];
        p8 s_ap_traffic[32];
        p8 c_key[16];
        p8 s_key[16];
        p8 c_iv[12];
        p8 s_iv[12];
        crypto_aesgcm_key c_gcm;
        crypto_aesgcm_key s_gcm;
        p64 seq_read;
        p64 seq_write;
        p8 leaf_qx[48];
        p8 leaf_qy[48];
        p8 leaf_n[512];
        positive leaf_n_length;
        p64 leaf_e;
        p8 leaf_curve;
        /* A post-handshake message may span records and reads; close_notify,
           once read, answers every later read. */
        bool closed;
        positive post_handshake_used;
        p8 post_handshake[TLS_HS_MAX];
        /* Socket bytes. Records not yet opened lie in
           [receive_start, receive_end); application data is decrypted where
           it lies, and plain_at and plain_used name the part of the last
           opened record not yet handed out. Opened plaintext stays until a
           later receive overwrites it or tls_forget erases the connection. */
        positive receive_start;
        positive receive_end;
        positive plain_at;
        positive plain_used;
        p8 receive[TLS_RECEIVE_ROOM];
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

/* A record leaves in one send. Sent in pieces, everything after the first
   piece waits under Nagle for the peer to acknowledge it. */
static bipolar tls_send_plain(tls_conn address_to tls, p8 type, p8 address_to body,
                              positive length)
{
        p8 record[5 + TLS_HS_MAX];

        if (length > TLS_HS_MAX)
                return TLS_FAIL;
        record[0] = type;
        record[1] = 0x03;
        record[2] = 0x03;
        record[3] = (p8)(length >> 8);
        record[4] = (p8)length;
        memory_copy(record + 5, body, length);
        return network_stream_send_all(tls->handle, record, 5 + length)
                   ? TLS_OK : TLS_FAIL;
}

static bipolar tls_send_enc(tls_conn address_to tls, p8 inner_type,
                            p8 address_to body, positive length)
{
        p8 record[5 + TLS_RECORD_MAX];
        p8 address_to header = record;
        p8 address_to inner = record + 5;
        p8 nonce[12];
        p8 tag[16];
        p8 aad[5];
        positive inner_length = 0;
        positive record_length = 0;
        bipolar status = TLS_FAIL;

        if (length > TLS_RECORD_MAX - 17 ||
            tls->seq_write >= TLS_AES_GCM_RECORD_LIMIT)
                goto done;
        inner_length = length + 1;
        record_length = inner_length + 16;

        memory_copy(inner, body, length);
        inner[length] = inner_type;

        header[0] = TLS_CT_APP;
        header[1] = 0x03;
        header[2] = 0x03;
        header[3] = (p8)(record_length >> 8);
        header[4] = (p8)record_length;
        memory_copy(aad, header, 5);

        tls_nonce(tls->c_iv, tls->seq_write, nonce);
        crypto_aesgcm_seal(address_of tls->c_gcm, nonce, aad, 5, inner, inner_length,
                              tag);
        tls->seq_write++;

        memory_copy(inner + inner_length, tag, 16);
        status = network_stream_send_all(tls->handle, record, 5 + record_length)
                     ? TLS_OK : TLS_FAIL;

done:
        if (record_length)
                crypto_forget(record, 5 + record_length);
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

        // The record layer opens records where they lie: inner is payload.
        if (inner != payload)
                memory_copy(inner, payload, payload_length - 16);
        memory_copy(tag, payload + payload_length - 16, 16);
        tls_nonce(tls->s_iv, tls->seq_read, nonce);
        if (!crypto_aesgcm_open(address_of tls->s_gcm, nonce, aad, 5, inner,
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

/* Receive behind receive_end. Opened bytes before receive_start are dropped:
   for free when nothing unopened remains, and otherwise the unopened tail
   moves to the front only when the room behind it could not hold a whole
   record, so a record arriving in pieces is not moved again per piece. The
   read is tried before any wait, because mid-transfer the socket almost
   always has bytes queued; only an empty socket polls, under the deadline,
   and nothing blocks past it. */
static bool tls_receive(tls_conn address_to tls,
                        const network_deadline address_to deadline)
{
        positive have = tls->receive_end - tls->receive_start;
        positive room;
        bipolar got;

        if (!have)
        {
                tls->receive_start = 0;
                tls->receive_end = 0;
        }
        else if (sizeof(tls->receive) - tls->receive_end < 5 + TLS_RECORD_MAX)
        {
                memory_copy(tls->receive, tls->receive + tls->receive_start,
                            have);
                tls->receive_start = 0;
                tls->receive_end = have;
        }
        room = sizeof(tls->receive) - tls->receive_end;

        do
        {
                p8 address_to into = tls->receive + tls->receive_end;

                if (!deadline)
                        got = system_read_retry((positive)tls->handle, into,
                                                room);
                else
                {
                        got = socket_receive((b32)tls->handle, into, room,
                                             MSG_DONTWAIT, null, 0);
                        if (got == NETWORK_TRY_AGAIN)
                                got = network_stream_read_some_until(
                                    tls->handle, into, room, deadline);
                }
        } while (got == NETWORK_INTERRUPTED);

        if (got <= 0 || (positive)got > room)
                return false;
        tls->receive_end += (positive)got;
        return true;
}

static bool tls_record_whole(tls_conn address_to tls)
{
        positive have = tls->receive_end - tls->receive_start;
        p8 address_to header = tls->receive + tls->receive_start;

        return have >= 5 &&
               have - 5 >= (((positive)header[3] << 8) | header[4]);
}

/* Open the next record, receiving until it is whole. Its bytes stay in the
   receive buffer: *inner points at the plaintext, decrypted in place, and is
   valid until the next receive. */
static bipolar tls_next_record(tls_conn address_to tls, p8 address_to type,
                               p8 address_to address_to inner,
                               positive address_to length,
                               const network_deadline address_to deadline)
{
        p8 address_to header;
        p8 address_to payload;
        positive payload_length = 0;
        positive inner_length = 0;
        p8 inner_type = 0;

        for (;;)
        {
                positive have = tls->receive_end - tls->receive_start;

                header = tls->receive + tls->receive_start;
                if (have >= 5)
                {
                        /* TLS 1.3 authenticates these bytes as AAD for
                           encrypted records and fixes legacy_record_version
                           at TLS 1.2 for every server record. */
                        if (!tls_record_version_valid(header))
                                return TLS_FAIL;
                        payload_length = ((positive)header[3] << 8) | header[4];
                        if (!payload_length || payload_length > TLS_RECORD_MAX)
                                return TLS_FAIL;
                        if (have - 5 >= payload_length)
                                break;
                }
                if (!tls_receive(tls, deadline))
                        return TLS_FAIL;
        }

        payload = header + 5;
        tls->receive_start += 5 + payload_length;

        if (header[0] == TLS_CT_CCS)
        {
                if (!tls_compatibility_ccs_valid(payload, payload_length,
                                                 tls->application))
                        return TLS_FAIL;
                address_to type = TLS_CT_CCS;
                address_to inner = payload;
                address_to length = 0;
                return TLS_OK;
        }

        if (!tls->encrypted)
        {
                if (header[0] != TLS_CT_HANDSHAKE)
                        return TLS_FAIL;
                address_to type = TLS_CT_HANDSHAKE;
                address_to inner = payload;
                address_to length = payload_length;
                return TLS_OK;
        }

        if (header[0] != TLS_CT_APP ||
            tls_decrypt_record(tls, payload, payload_length, header, payload,
                               address_of inner_length, address_of inner_type))
                return TLS_FAIL;

        if (inner_type == TLS_CT_ALERT)
                return inner_length == 2 && payload[1] == 0 ? TLS_EOF
                                                            : TLS_FAIL;

        address_to type = inner_type;
        address_to inner = payload;
        address_to length = inner_length;
        return TLS_OK;
}

/* The handshake's copy of one record. */
static bipolar tls_read_record(tls_conn address_to tls, p8 address_to type,
                               p8 address_to body, positive room,
                               positive address_to length,
                               const network_deadline address_to deadline)
{
        p8 address_to inner = null;
        positive inner_length = 0;
        bipolar status = tls_next_record(tls, type, address_of inner,
                                         address_of inner_length, deadline);

        if (status)
                return status;
        if (inner_length > room)
                return TLS_FAIL;
        memory_copy(body, inner, inner_length);
        crypto_forget(inner, inner_length);
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

static bool tls_oid_is(const p8 address_to bytes, positive length,
                       const p8 address_to oid, positive oid_length)
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
                         tls_oid_sha256_rsa, sizeof tls_oid_sha256_rsa) ||
              tls_oid_is(der + oid_at, oid_stop - oid_at,
                         tls_oid_sha384_rsa, sizeof tls_oid_sha384_rsa);
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

/* The standard base64 alphabet without line breaks; the byte count, or 0
   for any other character or more than room bytes. */
static positive tls_base64_decode(p8 address_to out, positive room,
                                  string_address text)
{
        p32 bits = 0;
        positive have = 0;
        positive count = 0;

        for (; *text && *text != '='; text++)
        {
                p8 c = (p8)*text;
                p32 value;

                if (c >= 'A' && c <= 'Z')
                        value = c - 'A';
                else if (c >= 'a' && c <= 'z')
                        value = c - 'a' + 26;
                else if (c >= '0' && c <= '9')
                        value = c - '0' + 52;
                else if (c == '+')
                        value = 62;
                else if (c == '/')
                        value = 63;
                else
                        return 0;
                bits = (bits << 6) | value;
                have += 6;
                if (have >= 8)
                {
                        have -= 8;
                        if (count == room)
                                return 0;
                        out[count++] = (p8)(bits >> have);
                }
        }

        return count;
}

/* An anchor's key laid out the way tls_parse_cert lays out a served one. */
static bool tls_anchor_key(const tls_anchor address_to anchor,
                           tls_cert address_to root)
{
        p8 key[512];
        positive length = tls_base64_decode(key, sizeof key, anchor->key);
        positive coord = length / 2;

        memory_fill(root, 0, sizeof(*root));
        if (!length || length != anchor->key_length)
                return false;
        root->curve = anchor->curve;
        if (anchor->curve == 3)
        {
                memory_copy(root->modulus, key, length);
                root->modulus_length = length;
                root->exponent = anchor->exponent;
                return true;
        }
        if ((anchor->curve != 1 || coord != 32) &&
            (anchor->curve != 2 || coord != 48))
                return false;
        memory_copy(root->qx + 48 - coord, key, coord);
        memory_copy(root->qy + 48 - coord, key + coord, coord);
        return true;
}

/* TLS_BENCH_ANCHOR names a file holding tls_bench_anchor_x and _y, the
   P-384 root that test/differential.py --harness https_bench generates for a
   loopback server. Only that harness defines it; build.sh never does, so a
   shipped binary trusts exactly the anchors in anchors.inc. */
#ifdef TLS_BENCH_ANCHOR
#include TLS_BENCH_ANCHOR
#endif

/* A served certificate carrying an anchor's key ends the chain, whoever
   signed it: the key hash finds candidates and the whole key decides. */
static bool tls_spki_is_anchor(tls_cert address_to cert)
{
        p8 key[96];
        p8 digest[32];
        positive coord = cert->curve == 1 ? 32 : 48;

        if (cert->curve == 3)
                crypto_sha256_of(cert->modulus, cert->modulus_length, digest);
        else if (cert->curve == 1 || cert->curve == 2)
        {
                memory_copy(key, cert->qx + 48 - coord, coord);
                memory_copy(key + coord, cert->qy + 48 - coord, coord);
                crypto_sha256_of(key, coord * 2, digest);
        }
        else
                return false;

#ifdef TLS_BENCH_ANCHOR
        if (cert->curve == 2 &&
            !memory_compare(cert->qx, tls_bench_anchor_x, 48) &&
            !memory_compare(cert->qy, tls_bench_anchor_y, 48))
                return true;
#endif

        for (positive i = 0; i < array_count(tls_anchors); i++)
        {
                tls_cert root;

                if (tls_anchors[i].curve != cert->curve ||
                    memory_compare(tls_anchors[i].key_hash, digest, 8) ||
                    !tls_anchor_key(tls_anchors + i, address_of root))
                        continue;
                if (cert->curve == 3
                        ? root.modulus_length == cert->modulus_length &&
                              root.exponent == cert->exponent &&
                              !memory_compare(root.modulus, cert->modulus,
                                              root.modulus_length)
                        : !memory_compare(root.qx, cert->qx, 48) &&
                              !memory_compare(root.qy, cert->qy, 48))
                        return true;
        }

        return false;
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
                if (issuer->curve != 1 && issuer->curve != 2)
                        return false;
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
                if (issuer->curve != 1 && issuer->curve != 2)
                        return false;
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
                if (issuer->curve != 3)
                        return false;
                crypto_sha256_of(child->tbs, child->tbs_length, hash);
                return crypto_rsa_pkcs1_sha256(issuer->modulus, issuer->modulus_length,
                                               issuer->exponent, child->sig,
                                               child->sig_length, hash);
        }

        if (tls_oid_is(child->sig_oid, child->sig_oid_length, tls_oid_sha384_rsa, 9))
        {
                if (issuer->curve != 3)
                        return false;
                crypto_sha384(child->tbs, child->tbs_length, hash);
                return crypto_rsa_pkcs1_sha384(issuer->modulus, issuer->modulus_length,
                                               issuer->exponent, child->sig,
                                               child->sig_length, hash);
        }

        return false;
}

/* The last certificate served names its issuer.  Each anchor with that
   subject Name is tried, and one signature that verifies ends the chain; a
   Name no anchor carries fails without any signature check. */
static bool tls_anchor_verifies(tls_cert address_to child)
{
        p8 digest[32];

        if (!child->issuer || !child->issuer_length)
                return false;
        crypto_sha256_of(child->issuer, child->issuer_length, digest);
        for (positive i = 0; i < array_count(tls_anchors); i++)
        {
                tls_cert root;

                if (memory_compare(tls_anchors[i].name, digest, 8) ||
                    !tls_anchor_key(tls_anchors + i, address_of root))
                        continue;
                if (tls_verify_one(child, address_of root))
                        return true;
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
                        return tls_anchor_verifies(certs + i);
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
            0, 0x0a, 0, 8, 0, 6, 0, 0x1d, 0, 0x17, 0, 0x18};
        static const p8 share_intro[] = {
            0, 0x33, 0, 208, 0, 206};
        static const p8 x25519_item[] = {0, 0x1d, 0, 32};
        static const p8 p256_item[] = {0, 0x17, 0, 65};
        static const p8 p384_item[] = {0, 0x18, 0, 97};
        static const p8 tail[] = {
            0, 0x2b, 0, 3, 2, 0x03, 0x04,
            0, 0x0d, 0, 8, 0, 6, 0x04, 0x03, 0x05, 0x03, 0x08, 0x04};
        p8 random[32];
        p8 x25519_public[32];
        p8 p256_public[65];
        p8 p384_public[97];
        p8 base[32];
        p8 raw[48];
        positive host_length = string_length(tls->host);
        bool named = string_to_host(tls->host) < 0;
        positive at = 0;
        positive ext_len_at;
        positive tries;
        bipolar status = TLS_FAIL;

        if (system_random_fill(random, 32, 0) < 0)
                goto done;
        if (system_random_fill(tls->x25519_scalar, 32, 0) < 0)
                goto done;

        memory_fill(base, 0, 32);
        base[0] = 9;
        if (!crypto_x25519(x25519_public, tls->x25519_scalar, base))
                goto done;

        for (tries = 0;; tries++)
        {
                if (tries > 8 || system_random_fill(raw, 32, 0) < 0)
                        goto done;
                if (crypto_scalar_reduce_be(tls->p256_scalar, raw, 32,
                                            crypto_p256_n, 4))
                        break;
        }
        if (!crypto_ecdh_p256_public(p256_public, tls->p256_scalar))
                goto done;

        for (tries = 0;; tries++)
        {
                if (tries > 8 || system_random_fill(raw, 48, 0) < 0)
                        goto done;
                if (crypto_scalar_reduce_be(tls->p384_scalar, raw, 48,
                                            crypto_p384_n, 6))
                        break;
        }
        if (!crypto_ecdh_p384_public(p384_public, tls->p384_scalar))
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
            !tls_hello_append(out, room, address_of at, share_intro,
                              sizeof share_intro) ||
            !tls_hello_append(out, room, address_of at, x25519_item,
                              sizeof x25519_item) ||
            !tls_hello_append(out, room, address_of at, x25519_public,
                              sizeof x25519_public) ||
            !tls_hello_append(out, room, address_of at, p256_item,
                              sizeof p256_item) ||
            !tls_hello_append(out, room, address_of at, p256_public,
                              sizeof p256_public) ||
            !tls_hello_append(out, room, address_of at, p384_item,
                              sizeof p384_item) ||
            !tls_hello_append(out, room, address_of at, p384_public,
                              sizeof p384_public) ||
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
        crypto_forget(x25519_public, sizeof x25519_public);
        crypto_forget(p256_public, sizeof p256_public);
        crypto_forget(p384_public, sizeof p384_public);
        crypto_forget(base, sizeof base);
        crypto_forget(raw, sizeof raw);
        if (status)
        {
                crypto_forget(tls->x25519_scalar, sizeof tls->x25519_scalar);
                crypto_forget(tls->p256_scalar, sizeof tls->p256_scalar);
                crypto_forget(tls->p384_scalar, sizeof tls->p384_scalar);
        }
        return status;
}

static bipolar tls_server_hello_keys(p8 address_to hello, positive length,
                                     p8 address_to peer, positive room,
                                     positive address_to share_length,
                                     positive address_to group)
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
                        positive named;
                        positive klen;

                        if (seen_share || elen < 4)
                                return TLS_FAIL;
                        named = ((positive)hello[at] << 8) | hello[at + 1];
                        klen = ((positive)hello[at + 2] << 8) | hello[at + 3];
                        if (4 + klen != elen)
                                return TLS_FAIL;
                        if (!((named == 0x001d && klen == 32) ||
                              (named == 0x0017 && klen == 65) ||
                              (named == 0x0018 && klen == 97)))
                                return TLS_FAIL;
                        if (klen > room)
                                return TLS_FAIL;
                        memory_copy(peer, hello + at + 4, klen);
                        address_to share_length = klen;
                        address_to group = named;
                        seen_share = true;
                }
                else
                        return TLS_FAIL;

                at += elen;
        }

        return seen_version && seen_share ? TLS_OK : TLS_FAIL;
}

static bipolar tls_server_hello_share(p8 address_to hello, positive length,
                                      p8 address_to peer)
{
        p8 key[97];
        positive n = 0;
        positive group = 0;

        if (tls_server_hello_keys(hello, length, key, sizeof key, address_of n,
                                  address_of group))
                return TLS_FAIL;
        if (group != 0x001d || n != 32)
                return TLS_FAIL;

        memory_copy(peer, key, 32);
        return TLS_OK;
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

static bipolar tls_install_handshake_keys(tls_conn address_to tls,
                                          p8 address_to shared,
                                          positive shared_length)
{
        p8 early[32];
        p8 zeros[32];
        p8 derived[32];
        p8 empty[32];

        memory_fill(zeros, 0, 32);
        tls_empty_hash(empty);
        crypto_hkdf_extract(zeros, 32, zeros, 32, early);
        tls_expand_label(early, "derived", empty, 32, derived, 32);
        crypto_hkdf_extract(derived, 32, shared, shared_length, tls->hs_secret);
        tls_derive_secret(tls->hs_secret, "c hs traffic",
                          address_of tls->transcript, tls->c_hs_traffic);
        tls_derive_secret(tls->hs_secret, "s hs traffic",
                          address_of tls->transcript, tls->s_hs_traffic);
        tls_traffic_keys(tls->c_hs_traffic, tls->c_key, tls->c_iv);
        tls_traffic_keys(tls->s_hs_traffic, tls->s_key, tls->s_iv);
        crypto_aesgcm_prepare(address_of tls->c_gcm, tls->c_key);
        crypto_aesgcm_prepare(address_of tls->s_gcm, tls->s_key);
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
        crypto_aesgcm_prepare(address_of tls->c_gcm, tls->c_key);
        crypto_aesgcm_prepare(address_of tls->s_gcm, tls->s_key);
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
        p8 peer[97];
        p8 shared[48];
        positive hello_length = 0;
        p8 type = 0;
        positive length = 0;
        p8 hs[TLS_HS_MAX];
        positive hs_used = 0;
        p8 flight = TLS_SERVER_FLIGHT_EE;
        bool seen_ccs = false;
        bipolar status = TLS_FAIL;
        positive share_length = 0;
        positive group = 0;

        crypto_sha256_open(address_of tls->transcript);
        tls->receive_start = 0;
        tls->receive_end = 0;
        tls->plain_used = 0;
        tls->closed = false;
        tls->post_handshake_used = 0;
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
        if (tls_server_hello_keys(hs, hs_used, peer, sizeof peer,
                                  address_of share_length, address_of group))
                goto done;
        hs_used = 0;

        if (group == 0x001d)
        {
                if (share_length != 32 ||
                    !crypto_x25519(shared, tls->x25519_scalar, peer))
                        goto done;
                if (tls_install_handshake_keys(tls, shared, 32))
                        goto done;
        }
        else if (group == 0x0017)
        {
                if (share_length != 65 ||
                    !crypto_ecdh_p256_shared(shared, tls->p256_scalar, peer))
                        goto done;
                if (tls_install_handshake_keys(tls, shared, 32))
                        goto done;
        }
        else if (group == 0x0018)
        {
                if (share_length != 97 ||
                    !crypto_ecdh_p384_shared(shared, tls->p384_scalar, peer))
                        goto done;
                if (tls_install_handshake_keys(tls, shared, 48))
                        goto done;
        }
        else
                goto done;
        crypto_forget(tls->x25519_scalar, sizeof tls->x25519_scalar);
        crypto_forget(tls->p256_scalar, sizeof tls->p256_scalar);
        crypto_forget(tls->p384_scalar, sizeof tls->p384_scalar);
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
        crypto_forget(tls->x25519_scalar, sizeof tls->x25519_scalar);
        crypto_forget(tls->p256_scalar, sizeof tls->p256_scalar);
        crypto_forget(tls->p384_scalar, sizeof tls->p384_scalar);
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

/* Up to room bytes of the next application data, lent rather than copied:
   *span points into the receive buffer and stays valid until a later read on
   this connection receives; *got of zero is close_notify, and stays so. Given
   a deadline, every wait shares it. Given seconds or nanoseconds instead, a
   deadline that long starts the first time a wait is needed, so records
   already whole in the buffer are opened without asking the clock. Neither
   renews: tickets, empty records and partial records all spend one budget.
   hold never receives: when the next record is not yet whole it answers
   TLS_AGAIN, so a writer can gather every record already here while the
   spans it holds stay put. */
static bipolar tls_take(tls_conn address_to tls, positive room,
                        p8 address_to address_to span, positive address_to got,
                        const network_deadline address_to deadline,
                        positive seconds, positive nanoseconds, bool hold)
{
        network_deadline patience;
        bool waiting = !seconds && !nanoseconds;
        p8 type = 0;
        p8 address_to inner = null;
        positive length = 0;
        bipolar status;

        if (tls->plain_used)
        {
                positive take = min(room, tls->plain_used);

                address_to span = tls->receive + tls->plain_at;
                tls->plain_at += take;
                tls->plain_used -= take;
                address_to got = take;
                return TLS_OK;
        }

        for (;;)
        {
                if (tls->closed)
                {
                        address_to got = 0;
                        return tls->post_handshake_used ? TLS_FAIL : TLS_OK;
                }
                if (!tls_record_whole(tls))
                {
                        if (hold)
                                return TLS_AGAIN;
                        if (!waiting)
                        {
                                if (!network_deadline_begin(address_of patience,
                                                            seconds,
                                                            nanoseconds))
                                        return TLS_FAIL;
                                deadline = address_of patience;
                                waiting = true;
                        }
                }
                status = tls_next_record(tls, address_of type, address_of inner,
                                         address_of length, deadline);
                if (status == TLS_EOF)
                {
                        tls->closed = true;
                        continue;
                }
                if (status || type == TLS_CT_CCS)
                        return TLS_FAIL;
                if (type == TLS_CT_HANDSHAKE)
                {
                        if (tls_post_handshake_append(
                                tls->post_handshake,
                                address_of tls->post_handshake_used,
                                inner, length))
                                return TLS_FAIL;
                        crypto_forget(inner, length);
                        continue;
                }
                if (type != TLS_CT_APP || tls->post_handshake_used)
                        return TLS_FAIL;
                if (!length)
                        continue;
                if (room > length)
                        room = length;
                address_to span = inner;
                address_to got = room;
                tls->plain_at = (positive)(inner - tls->receive) + room;
                tls->plain_used = length - room;
                return TLS_OK;
        }
}

static bipolar tls_borrow(tls_conn address_to tls, positive room,
                          p8 address_to address_to span,
                          positive address_to got, positive seconds,
                          positive nanoseconds)
{
        return tls_take(tls, room, span, got, null, seconds, nanoseconds,
                        false);
}

/* The next application data already whole in the receive buffer, lent
   without receiving; TLS_AGAIN when none is. */
static bipolar tls_lend(tls_conn address_to tls, positive room,
                        p8 address_to address_to span, positive address_to got)
{
        return tls_take(tls, room, span, got, null, 0, 0, true);
}

static bipolar tls_read_until(
    tls_conn address_to tls, p8 address_to into, positive room,
    positive address_to got, const network_deadline address_to deadline)
{
        p8 address_to span = null;
        bipolar status = tls_take(tls, room, address_of span, got, deadline,
                                  0, 0, false);

        if (!status && address_to got)
                memory_copy(into, span, address_to got);
        return status;
}

static bipolar tls_read(tls_conn address_to tls, p8 address_to into,
                        positive room, positive address_to got)
{
        return tls_read_until(tls, into, room, got, null);
}

#endif
