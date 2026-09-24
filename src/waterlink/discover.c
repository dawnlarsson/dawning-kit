/*
        Finding the other members of a group on the local network, over
        mDNS and DNS-SD -- the protocol printers announce themselves with, so
        `dns-sd -B _waterlink._udp` on a Mac or avahi-browse on Linux lists a
        machine that is listening.

        What is announced says that a waterlink machine is here and nothing
        about which one it is or which group it belongs to. Every group a
        machine has joined is an instance under _waterlink._udp.local named
        by a random label, pointing at a random host label, with a TXT record
        of three fields:

                n=  sixteen random bytes, the nonce
                t=  HMAC of the nonce under the group's discovery key
                w=  HMAC of the nonce and this machine's public key

        A member of the group recomputes t and knows another member is there;
        anyone else sees random bytes, and fresh ones at every boot and every
        hour, so a machine cannot be followed from one to the next by what it
        says. w tells a member which of the machines it already paired with
        this is, so a moved address is corrected without pairing again. No
        time goes into either: two headless boxes that have not set their
        clocks yet still recognise each other, and a replayed announcement
        buys nothing past a pairing attempt that fails.

        THE SECRET

        A group is a namespace and a secret, and every key comes from the two
        of them through PBKDF2-HMAC-SHA256, the namespace in the salt, at
        WATERLINK_GROUP_ROUNDS iterations -- a protocol constant, since every
        member must arrive at the same key. That is the only thing standing
        between a weak secret and anyone on the network: the t field is an
        HMAC under a key derived from it, broadcast to all, so a listener can
        try guesses offline without ever sending a thing, at the price of one
        derivation a guess: 56 ms on one core of a 9950X, and a graphics card
        running PBKDF2 does thousands a second, so a word a person chose will
        fall to anyone who cares to try. A secret `moonwater link join` makes
        itself has 160 random bits, and no guessing reaches it.

        The records are parsed the way the most exposed parser on the machine
        must be: every length checked against what is left, a compression
        pointer followed only backwards and at most a few times, a name held
        to 255 bytes and a label to 63, label types other than plain refused,
        and nothing taken from a record that another check still depends on.
        An address is never read out of a record at all -- the A record and
        the SRV target are whatever the sender wrote -- so the caller connects
        to the packet's own source, on the port the SRV names.

        Like link.c this is a transform; the caller owns the socket.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_DISCOVER_INCLUDED
#define WATERLINK_DISCOVER_INCLUDED

#include "waterlink.c"

#define WATERLINK_MDNS_PORT 5353
#define WATERLINK_MDNS_GROUP 0xe00000fbu // 224.0.0.251
#define WATERLINK_MDNS_MAX 1500

#define WATERLINK_NAMESPACE_MAX 32
#define WATERLINK_NONCE_BYTES 16
#define WATERLINK_TAG_BYTES_SHORT 16

/*      Every member derives the same key, so this is part of the protocol:
        change it and machines built before and after cannot pair. OWASP's
        2023 figure for PBKDF2-HMAC-SHA256. */
#ifndef WATERLINK_GROUP_ROUNDS
#define WATERLINK_GROUP_ROUNDS 600000
#endif

// The service, the type every DNS-SD browser is asked about.
static const p8 waterlink_service_name[] = "\x0a_waterlink\x04_udp\x05local";
#define WATERLINK_SERVICE_BYTES (sizeof(waterlink_service_name) - 1 + 1)

struct waterlink_group_keys {
        p8 group[32];    // PBKDF2 of namespace and secret
        p8 discover[32]; // keys t and w
        p8 pair[32];     // the pairing handshake's PSK
        p32 mark;        // what a peer record paired by this group carries
};

/*
        PBKDF2-HMAC-SHA256, one block of output, the salt naming the protocol
        and the namespace so no two groups share a derivation.
*/
fn waterlink_group_derive(string_address namespace, p8 address_to secret,
                          positive secret_length, positive rounds,
                          p8 address_to out)
{
        p8 salt[18 + WATERLINK_NAMESPACE_MAX];
        positive namespace_length = string_length(namespace);

        if (namespace_length > WATERLINK_NAMESPACE_MAX)
                namespace_length = WATERLINK_NAMESPACE_MAX;
        memory_copy(salt, "waterlink group 1 ", 18);
        memory_copy(salt + 18, namespace, namespace_length);
        crypto_pbkdf2(DIGEST_SHA256, 32, secret, secret_length, salt,
                      18 + namespace_length, rounds, out, 32);
}

fn waterlink_group_keys_from(struct waterlink_group_keys address_to keys,
                             p8 address_to group, string_address namespace)
{
        p8 hashed[32];

        memory_copy(keys->group, group, 32);
        crypto_hkdf_expand(group, (p8 address_to) "waterlink discover", 18,
                           keys->discover, 32);
        crypto_hkdf_expand(group, (p8 address_to) "waterlink pair", 14,
                           keys->pair, 32);

        //      A peer record keeps which group paired it as 32 bits of the
        //      namespace's hash, never zero: zero is paired by hand.
        {
                crypto_sha256 hash;

                crypto_sha256_open(address_of hash);
                crypto_sha256_write(address_of hash,
                                    (p8 address_to) "waterlink mark ", 15);
                crypto_sha256_write(address_of hash, (p8 address_to)namespace,
                                    string_length(namespace));
                crypto_sha256_close(address_of hash, hashed);
        }
        memory_copy(address_of keys->mark, hashed, 4);
        if (!keys->mark)
                keys->mark = 1;
}

fn waterlink_tag(struct waterlink_group_keys address_to keys,
                 p8 address_to nonce, p8 address_to tag)
{
        crypto_mac mac;
        p8 full[32];

        crypto_hmac_open(address_of mac, DIGEST_SHA256, 32, keys->discover, 32);
        crypto_hmac_write(address_of mac, (p8 address_to) "tag", 3);
        crypto_hmac_write(address_of mac, nonce, WATERLINK_NONCE_BYTES);
        crypto_hmac_close(address_of mac, full);
        memory_copy(tag, full, WATERLINK_TAG_BYTES_SHORT);
}

fn waterlink_who(struct waterlink_group_keys address_to keys,
                 p8 address_to nonce, p8 address_to public, p8 address_to who)
{
        crypto_mac mac;
        p8 full[32];

        crypto_hmac_open(address_of mac, DIGEST_SHA256, 32, keys->discover, 32);
        crypto_hmac_write(address_of mac, (p8 address_to) "who", 3);
        crypto_hmac_write(address_of mac, nonce, WATERLINK_NONCE_BYTES);
        crypto_hmac_write(address_of mac, public, 32);
        crypto_hmac_close(address_of mac, full);
        memory_copy(who, full, WATERLINK_TAG_BYTES_SHORT);
}

// Writing ---------------------------------------------------------------

typedef struct
{
        p8 address_to at;
        positive used;
        positive room;
        bool full;
} waterlink_dns_writer;

static fn waterlink_dns_put(waterlink_dns_writer address_to out,
                            const p8 address_to bytes, positive length)
{
        if (out->used + length > out->room)
        {
                out->full = true;
                return;
        }
        memory_copy(out->at + out->used, bytes, length);
        out->used += length;
}

static fn waterlink_dns_put16(waterlink_dns_writer address_to out, p32 value)
{
        p8 two[2] = {(p8)(value >> 8), (p8)value};

        waterlink_dns_put(out, two, 2);
}

static fn waterlink_dns_put32(waterlink_dns_writer address_to out, p32 value)
{
        waterlink_dns_put16(out, value >> 16);
        waterlink_dns_put16(out, value);
}

static const char waterlink_hex[] = "0123456789abcdef";


/*
        One machine's announcement, or with ttl 0 its goodbye: for each group
        an instance -- PTR from the service, SRV to the host on the port, TXT
        with the three fields -- and the host's A record when there is an
        address to give. The instance label is "wl-" and twenty hex digits,
        the host label "wl-" and twelve, both random and chosen by the caller.
        Returns the length, or 0 when it did not fit.
*/
struct waterlink_announce_group {
        p8 instance[10]; // random
        p8 nonce[WATERLINK_NONCE_BYTES];
        p8 tag[WATERLINK_TAG_BYTES_SHORT];
        p8 who[WATERLINK_TAG_BYTES_SHORT];
};

positive waterlink_mdns_announce(p8 address_to packet, positive room,
                                 struct waterlink_announce_group address_to groups,
                                 positive count, p8 address_to host_bytes,
                                 p16 port, p32 address, p32 ttl, p16 id,
                                 p8 address_to question, positive question_length)
{
        waterlink_dns_writer out = {packet, 0, room, false};
        positive service_at;
        positive host_at = 0;
        p8 name[24];

        //      A reply to a one-shot query carries its ID and its question.
        waterlink_dns_put16(address_of out, id);
        waterlink_dns_put16(address_of out, 0x8400);
        waterlink_dns_put16(address_of out, question ? 1 : 0);
        waterlink_dns_put16(address_of out, count * 3 + (address ? 1 : 0));
        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put16(address_of out, 0);
        if (question)
                waterlink_dns_put(address_of out, question, question_length);

        service_at = out.used;
        for (positive at = 0; at < count; at++)
        {
                struct waterlink_announce_group address_to group = groups + at;
                positive instance_at;
                p8 txt[3 * (2 + 2 * 16) + 8];
                positive txt_used = 0;

                //      PTR: the service, to the instance.
                if (!at)
                        waterlink_dns_put(address_of out, waterlink_service_name,
                                          WATERLINK_SERVICE_BYTES);
                else
                        waterlink_dns_put16(address_of out,
                                            0xc000 | (p32)service_at);
                waterlink_dns_put16(address_of out, 12);
                waterlink_dns_put16(address_of out, 1);
                waterlink_dns_put32(address_of out, ttl ? 4500 : 0);
                waterlink_dns_put16(address_of out, 1 + 23 + 2);
                instance_at = out.used;
                name[0] = 23;
                memory_copy(name + 1, "wl-", 3);
                memory_into_hex(name + 4, group->instance, 10);
                waterlink_dns_put(address_of out, name, 24);
                waterlink_dns_put16(address_of out, 0xc000 | (p32)service_at);

                //      SRV: the instance, to the host, on the port.
                waterlink_dns_put16(address_of out, 0xc000 | (p32)instance_at);
                waterlink_dns_put16(address_of out, 33);
                waterlink_dns_put16(address_of out, 0x8001);
                waterlink_dns_put32(address_of out, ttl);
                if (!host_at)
                {
                        waterlink_dns_put16(address_of out, 6 + 1 + 15 + 7);
                        waterlink_dns_put16(address_of out, 0);
                        waterlink_dns_put16(address_of out, 0);
                        waterlink_dns_put16(address_of out, port);
                        host_at = out.used;
                        name[0] = 15;
                        memory_copy(name + 1, "wl-", 3);
                        memory_into_hex(name + 4, host_bytes, 6);
                        waterlink_dns_put(address_of out, name, 16);
                        waterlink_dns_put(address_of out,
                                          (p8 address_to) "\x05local", 7);
                }
                else
                {
                        waterlink_dns_put16(address_of out, 6 + 2);
                        waterlink_dns_put16(address_of out, 0);
                        waterlink_dns_put16(address_of out, 0);
                        waterlink_dns_put16(address_of out, port);
                        waterlink_dns_put16(address_of out,
                                            0xc000 | (p32)host_at);
                }

                //      TXT: the three fields, forty-four bytes of hex.
                txt[txt_used++] = 3;
                memory_copy(txt + txt_used, "v=1", 3);
                txt_used += 3;
                txt[txt_used++] = 2 + 32;
                memory_copy(txt + txt_used, "n=", 2);
                memory_into_hex(txt + txt_used + 2, group->nonce, 16);
                txt_used += 34;
                txt[txt_used++] = 2 + 32;
                memory_copy(txt + txt_used, "t=", 2);
                memory_into_hex(txt + txt_used + 2, group->tag, 16);
                txt_used += 34;
                txt[txt_used++] = 2 + 32;
                memory_copy(txt + txt_used, "w=", 2);
                memory_into_hex(txt + txt_used + 2, group->who, 16);
                txt_used += 34;

                waterlink_dns_put16(address_of out, 0xc000 | (p32)instance_at);
                waterlink_dns_put16(address_of out, 16);
                waterlink_dns_put16(address_of out, 0x8001);
                waterlink_dns_put32(address_of out, ttl ? 4500 : 0);
                waterlink_dns_put16(address_of out, (p32)txt_used);
                waterlink_dns_put(address_of out, txt, txt_used);
        }

        if (address && host_at)
        {
                waterlink_dns_put16(address_of out, 0xc000 | (p32)host_at);
                waterlink_dns_put16(address_of out, 1);
                waterlink_dns_put16(address_of out, 0x8001);
                waterlink_dns_put32(address_of out, ttl);
                waterlink_dns_put16(address_of out, 4);
                waterlink_dns_put32(address_of out, address);
        }

        return out.full ? 0 : out.used;
}

// The question every browser and every member asks.
positive waterlink_mdns_query(p8 address_to packet, positive room)
{
        waterlink_dns_writer out = {packet, 0, room, false};

        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put16(address_of out, 1);
        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put16(address_of out, 0);
        waterlink_dns_put(address_of out, waterlink_service_name,
                          WATERLINK_SERVICE_BYTES);
        waterlink_dns_put16(address_of out, 12);
        waterlink_dns_put16(address_of out, 1);
        return out.full ? 0 : out.used;
}

// Reading ------------------------------------------------------------------

/*
        A name at offset at, flattened into labels as lengths and bytes the
        way it sits on the wire uncompressed. Pointers are followed only to
        somewhere before the label that points, which makes a loop impossible,
        and at most sixteen times. Returns the offset just past the name as it
        sits in the packet, or 0 for anything that is not a name.
*/
#define WATERLINK_NAME_BYTES 256

static positive waterlink_dns_name(const p8 address_to packet, positive length,
                                   positive at, p8 address_to into,
                                   positive address_to into_length)
{
        positive after = 0;
        positive used = 0;
        positive jumps = 0;
        positive floor = at;

        for (;;)
        {
                p8 first;

                if (at >= length)
                        return 0;
                first = packet[at];

                if ((first & 0xc0) == 0xc0)
                {
                        positive target;

                        if (at + 1 >= length || ++jumps > 16)
                                return 0;
                        target = ((positive)(first & 0x3f) << 8) | packet[at + 1];
                        if (!after)
                                after = at + 2;
                        //      Strictly backwards, and before every label
                        //      already read: nothing can be read twice.
                        if (target >= floor)
                                return 0;
                        at = target;
                        floor = target;
                        continue;
                }

                //      0x40 and 0x80 are extended and reserved label types.
                if (first & 0xc0)
                        return 0;

                if (!first)
                {
                        if (used + 1 > WATERLINK_NAME_BYTES - 1)
                                return 0;
                        into[used++] = 0;
                        address_to into_length = used;
                        return after ? after : at + 1;
                }

                if (at + 1 + first > length ||
                    used + 1 + first + 1 > WATERLINK_NAME_BYTES - 1)
                        return 0;
                memory_copy(into + used, packet + at, 1 + (positive)first);
                used += 1 + (positive)first;
                at += 1 + (positive)first;
        }
}

static p8 waterlink_ascii_lower(p8 c)
{
        return c >= 'A' && c <= 'Z' ? (p8)(c + 32) : c;
}

// Whether a flattened name is the service's, letter case aside.
static bool waterlink_is_service(const p8 address_to name, positive length)
{
        if (length != WATERLINK_SERVICE_BYTES)
                return false;
        for (positive at = 0; at < length; at++)
                if (waterlink_ascii_lower(name[at]) !=
                    waterlink_ascii_lower(waterlink_service_name[at]))
                        return false;
        return true;
}

// An instance of the service: one label and then the service's name.
static bool waterlink_instance_of(const p8 address_to name, positive length,
                                  p8 address_to label, positive address_to label_length)
{
        positive first = name[0];

        if (!first || 1 + first >= length ||
            !waterlink_is_service(name + 1 + first, length - 1 - first))
                return false;
        memory_copy(label, name + 1, first);
        address_to label_length = first;
        return true;
}

static bool waterlink_unhex(const p8 address_to text, positive count,
                            p8 address_to out)
{
        for (positive at = 0; at < count; at++)
        {
                p32 value = 0;

                for (positive half = 0; half < 2; half++)
                {
                        p8 c = waterlink_ascii_lower(text[2 * at + half]);
                        p32 digit = c >= '0' && c <= '9'   ? c - '0'
                                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                           : 16;

                        if (digit == 16)
                                return false;
                        value = value << 4 | digit;
                }
                out[at] = (p8)value;
        }
        return true;
}

#define WATERLINK_FOUND_MAX 8

struct waterlink_found_instance {
        p8 label[63];
        positive label_length;
        p16 port;
        bool has_port;
        bool has_fields;
        p8 nonce[WATERLINK_NONCE_BYTES];
        p8 tag[WATERLINK_TAG_BYTES_SHORT];
        p8 who[WATERLINK_TAG_BYTES_SHORT];
};

struct waterlink_found {
        p16 id;
        bool response;
        bool asked;     // a question for the service
        bool asked_once; // a one-shot question (the reply goes back unicast)
        p8 question[WATERLINK_NAME_BYTES + 4];
        positive question_length;
        positive count;
        struct waterlink_found_instance instance[WATERLINK_FOUND_MAX];
};

static struct waterlink_found_instance address_to
waterlink_found_at(struct waterlink_found address_to found,
                   const p8 address_to label, positive label_length)
{
        for (positive at = 0; at < found->count; at++)
                if (found->instance[at].label_length == label_length &&
                    !memory_compare(found->instance[at].label, label,
                                    label_length))
                        return found->instance + at;

        if (found->count == WATERLINK_FOUND_MAX)
                return null;
        memory_zero(found->instance + found->count,
                    sizeof(found->instance[0]));
        memory_copy(found->instance[found->count].label, label, label_length);
        found->instance[found->count].label_length = label_length;
        return found->instance + found->count++;
}

static fn waterlink_txt_fields(struct waterlink_found_instance address_to instance,
                               const p8 address_to rdata, positive length)
{
        positive at = 0;
        bool nonce = false, tag = false, who = false, version = false;

        while (at < length)
        {
                positive piece = rdata[at];
                const p8 address_to text = rdata + at + 1;

                if (at + 1 + piece > length)
                        return;
                if (piece == 3 && !memory_compare(text, "v=1", 3))
                        version = true;
                else if (piece == 34 && text[1] == '=')
                {
                        if (text[0] == 'n')
                                nonce = waterlink_unhex(text + 2, 16,
                                                        instance->nonce);
                        else if (text[0] == 't')
                                tag = waterlink_unhex(text + 2, 16,
                                                      instance->tag);
                        else if (text[0] == 'w')
                                who = waterlink_unhex(text + 2, 16,
                                                      instance->who);
                }
                at += 1 + piece;
        }

        instance->has_fields = version && nonce && tag && who;
}

/*
        Read one mDNS message: whether it asks about the service, and every
        instance of the service it describes with an SRV port and the three
        TXT fields. False when it is not a well formed message; a well formed
        one about other things answers true with nothing found.
*/
bool waterlink_mdns_read(const p8 address_to packet, positive length,
                         struct waterlink_found address_to found)
{
        p8 name[WATERLINK_NAME_BYTES];
        positive name_length;
        positive at = 12;
        p32 questions, records;

        memory_zero(found, sizeof(address_to found));
        if (length < 12 || length > WATERLINK_MDNS_MAX)
                return false;

        found->id = (p16)(packet[0] << 8 | packet[1]);
        found->response = (packet[2] & 0x80) != 0;
        //      Opcode, and rcode in a response, must be zero.
        if ((packet[2] & 0x78) || (found->response && (packet[3] & 0x0f)))
                return false;

        questions = (p32)packet[4] << 8 | packet[5];
        records = ((p32)packet[6] << 8 | packet[7]) +
                  ((p32)packet[8] << 8 | packet[9]) +
                  ((p32)packet[10] << 8 | packet[11]);

        //      Each question is at least five bytes and each record eleven:
        //      a count the packet cannot hold is refused before any is read.
        if (questions * 5 + records * 11 > length - 12)
                return false;

        for (p32 q = 0; q < questions; q++)
        {
                positive start = at;
                positive next = waterlink_dns_name(packet, length, at, name,
                                                   address_of name_length);
                p32 type;

                if (!next || next + 4 > length)
                        return false;
                type = (p32)packet[next] << 8 | packet[next + 1];
                if (!found->response && waterlink_is_service(name, name_length) &&
                    (type == 12 || type == 255))
                {
                        found->asked = true;
                        //      The question, kept whole and uncompressed, for
                        //      a reply that has to echo it.
                        if (next - start == name_length &&
                            name_length + 4 <= sizeof(found->question))
                        {
                                memory_copy(found->question, packet + start,
                                            name_length + 4);
                                found->question_length = name_length + 4;
                        }
                }
                at = next + 4;
        }

        for (p32 r = 0; r < records; r++)
        {
                positive next = waterlink_dns_name(packet, length, at, name,
                                                   address_of name_length);
                p32 type;
                positive rdlength;
                positive rdata;
                p8 label[63];
                positive label_length;
                struct waterlink_found_instance address_to instance;

                if (!next || next + 10 > length)
                        return false;
                type = (p32)packet[next] << 8 | packet[next + 1];
                rdlength = (positive)packet[next + 8] << 8 | packet[next + 9];
                rdata = next + 10;
                if (rdata + rdlength > length)
                        return false;
                at = rdata + rdlength;

                if (!found->response ||
                    !waterlink_instance_of(name, name_length, label,
                                           address_of label_length))
                        continue;

                if (type == 33 && rdlength >= 7)
                {
                        instance = waterlink_found_at(found, label,
                                                      label_length);
                        if (instance)
                        {
                                instance->port =
                                        (p16)(packet[rdata + 4] << 8 |
                                              packet[rdata + 5]);
                                instance->has_port = true;
                        }
                }
                else if (type == 16)
                {
                        instance = waterlink_found_at(found, label,
                                                      label_length);
                        if (instance)
                                waterlink_txt_fields(instance, packet + rdata,
                                                     rdlength);
                }
        }

        return at == length;
}

#endif // WATERLINK_DISCOVER_INCLUDED
