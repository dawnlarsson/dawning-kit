/*
        Groups, as the listener keeps them: announcing on the local network,
        recognising other members, pairing with the ones it does not know
        yet, and following the ones it does when their addresses change.

        A group is a line in /root/link.groups -- the namespace, the key
        derived from it and the secret (never the secret itself), a hash of
        the secret so a machine script that joins at every boot does not pay
        the derivation every boot, and the grants members get here. Written
        by `moonwater link join`, read by the listener whenever it changes.

        Discovery is IPv4 only for now, 224.0.0.251 on every interface that
        takes the membership; ff02::fb is not asked or answered. A packet is
        believed only if it arrived with the TTL it was sent with, 255, which
        no router forwards: that is what keeps this on the local link. An
        address is taken from where a packet came from, never from what it
        says.

        Included by service.c, which owns the sockets and the loop.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_NEARBY_INCLUDED
#define WATERLINK_NEARBY_INCLUDED

#include "discover.c"
#include "pair.c"

#define LINK_GROUPS_PATH "/root/link.groups"
#define LINK_GROUPS_NEXT "/root/link.groups.next"
#define LINK_PEERS_LOCK HOST_STATE "/link.peers.lock"
#define LINK_GROUPS_MAX 8
#define LINK_INTERFACES 16
#define LINK_PAIRING 8

#define LINK_ANNOUNCE_EVERY 60000000ull  // after the first three
#define LINK_ASK_EVERY 30000000ull
#define LINK_ROTATE_EVERY 3600000000ull  // new labels and nonces
#define LINK_PAIR_WAIT 3000000ull
#define LINK_PAIR_AGAIN 10000000ull      // the same place, not sooner

struct link_group_record {
        char namespace[WATERLINK_NAMESPACE_MAX];
        p8 key[32];   // PBKDF2 of namespace and secret
        p8 check[32]; // SHA-256 of the secret, salted: skips a repeat derivation
        p32 may;
        p32 flags;
        p8 reserved[24];
};

_Static_assert(sizeof(struct link_group_record) == 128,
               "a group record is 128 bytes");

typedef struct
{
        struct link_group_record record[LINK_GROUPS_MAX];
        positive count;
} link_groups;

fn link_groups_load(link_groups address_to groups)
{
        positive got = 0;

        memory_zero(groups, sizeof(address_to groups));
        if (link_read_private_records(LINK_GROUPS_PATH,
                                      (p8 address_to)groups->record,
                                      sizeof(groups->record),
                                      sizeof(struct link_group_record),
                                      address_of got) < 0)
                return;
        groups->count = got / sizeof(struct link_group_record);
        for (positive at = 0; at < groups->count; at++)
        {
                groups->record[at].namespace[WATERLINK_NAMESPACE_MAX - 1] = 0;
                if (!link_name_good(groups->record[at].namespace))
                {
                        groups->record[at] = groups->record[--groups->count];
                        at--;
                }
        }
}

static bipolar link_groups_save(link_groups address_to groups)
{
        bipolar failed = host_write_file(LINK_GROUPS_NEXT,
                                         (p8 address_to)groups->record,
                                         groups->count *
                                                 sizeof(struct link_group_record),
                                         0600, true);

        if (failed < 0)
                return failed;
        return system_rename_at(AT_FDCWD, LINK_GROUPS_NEXT, AT_FDCWD,
                                LINK_GROUPS_PATH, 0);
}

fn link_group_check(string_address namespace, p8 address_to secret,
                    positive length, p8 address_to check)
{
        crypto_sha256 hash;

        crypto_sha256_open(address_of hash);
        crypto_sha256_write(address_of hash, (p8 address_to) "waterlink check ",
                            16);
        crypto_sha256_write(address_of hash, (p8 address_to)namespace,
                            string_length(namespace));
        crypto_sha256_write(address_of hash, (p8 address_to) " ", 1);
        crypto_sha256_write(address_of hash, secret, length);
        crypto_sha256_close(address_of hash, check);
}

/*
        The peers file has two writers now, the command and the listener, so
        every load, change and save of it happens under this record lock.
*/
static bipolar link_peers_lock(void)
{
        link_record_lock lock = {LINK_F_WRLCK, 0, 0, 0, 0, 0, 0};
        bipolar handle;

        host_state_ready();
        handle = system_open_at_mode(AT_FDCWD, LINK_PEERS_LOCK,
                                     FILE_READ_WRITE | FILE_CREATE | O_NOFOLLOW |
                                             O_CLOEXEC,
                                     0600);
        if (handle < 0)
                return handle;
        //      F_SETLKW: wait for the other writer.
        while (system_call_3(syscall(fcntl), (positive)handle, 7,
                             (positive)address_of lock) == -4)
                ;
        return handle;
}

static fn link_peers_unlock(bipolar handle)
{
        if (handle >= 0)
                system_close(handle);
}

/*
        This machine's name as it offers it to a member: the host name, held
        to what link_name_good takes and short enough to leave room for a
        suffix. The far side checks it again all the same.
*/
fn link_machine_name(p8 address_to name)
{
        p8 uts[6 * 65];
        positive used = 0;

        memory_zero(name, WATERLINK_PAIR_NAME);
        if (system_call_1(syscall(uname), (positive)uts) >= 0)
                for (positive at = 65; at < 130 && uts[at] && used < 20; at++)
                {
                        p8 c = uts[at];
                        bool plain = (c >= 'a' && c <= 'z') ||
                                     (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9');

                        if (plain || (used && (c == '-' || c == '_')))
                                name[used++] = c;
                }
        if (!used)
                memory_copy(name, "machine", 8);
}

/*
        A member keeps a name that reads as the machine it is, and does not
        take one already in the file: a suffix from its key, the same every
        time it pairs, is added when it would.
*/
static fn link_name_for(link_peers address_to peers, p8 address_to offered,
                        p8 address_to key, p8 address_to name)
{
        p8 base[WATERLINK_NAME_MAX];
        positive length;

        offered[WATERLINK_PAIR_NAME - 1] = 0;
        memory_zero(base, sizeof base);
        length = string_length((string_address)offered);
        if (length > 20)
                length = 20;
        memory_copy(base, offered, length);
        if (!link_name_good((string_address)base))
                string_copy((string_address)base, "machine");

        string_copy((string_address)name, (string_address)base);
        for (positive width = 3; link_peer_named(peers, (string_address)name) &&
                                 width <= 8;
             width++)
        {
                positive used = string_length((string_address)base);

                memory_copy(name, base, used);
                name[used++] = '-';
                for (positive at = 0; at < width; at++)
                        name[used++] = (p8)waterlink_hex[key[at] >> 4];
                name[used] = 0;
        }
}

// Keeping the peer a pairing met. Never replaces a key already known.
static bool link_pair_keep(struct link_group_record address_to group,
                           struct waterlink_group_keys address_to keys,
                           p8 address_to key, p8 address_to offered,
                           p8 address_to address, p16 port)
{
        link_peers peers;
        struct waterlink_peer peer;
        bipolar lock = link_peers_lock();
        bool kept = false;

        link_peers_load(address_of peers);
        if (!link_peer_keyed(address_of peers, key) &&
            peers.count < LINK_PEERS_MAX &&
            !crypto_same(key, link_self.me.public, 32))
        {
                memory_zero(address_of peer, sizeof peer);
                memory_copy(peer.key, key, 32);
                link_name_for(address_of peers, offered, key,
                              (p8 address_to)peer.name);
                peer.may = group->may ? group->may : WATERLINK_MAY_DEFAULT;
                peer.group = keys->mark;
                memory_copy(peer.address, address, 16);
                peer.port = port;
                peers.peer[peers.count++] = peer;
                kept = link_peers_save(address_of peers) >= 0;
        }
        link_peers_unlock(lock);
        link_self.state_dirty = true;
        return kept;
}

// A known member announced from somewhere new: the record follows it.
static fn link_peer_moved(p8 address_to key, p8 address_to address, p16 port)
{
        link_peers peers;
        struct waterlink_peer address_to peer;
        bipolar lock = link_peers_lock();

        link_peers_load(address_of peers);
        peer = link_peer_keyed(address_of peers, key);
        if (peer && (memory_compare(peer->address, address, 16) ||
                     peer->port != port))
        {
                memory_copy(peer->address, address, 16);
                peer->port = port;
                (void)link_peers_save(address_of peers);
        }
        link_peers_unlock(lock);
}

// The listener's side of it ------------------------------------------------

struct link_pairing {
        bool used;
        bool initiator;
        positive group;
        p32 ours;
        p32 theirs;
        p8 address[16];
        p16 port;
        p64 started;
        struct waterlink_noise noise;
};

struct link_attempt {
        p8 address[16];
        p16 port;
        p64 at;
};

typedef struct
{
        link_groups groups;
        struct waterlink_group_keys keys[LINK_GROUPS_MAX];
        struct waterlink_announce_group announce[LINK_GROUPS_MAX];
        p8 host[6];
        p64 groups_changed; // inode and size of the file as last read
        p64 looked;
        bipolar socket;
        b32 interface[LINK_INTERFACES];
        p32 interface_address[LINK_INTERFACES];
        positive interfaces;
        p64 next_announce;
        p64 next_ask;
        p64 rotated;
        positive announced;
        positive asked;
        p64 last_answer;
        p64 interfaces_looked;
        bool labels_ready;
        struct link_pairing pairing[LINK_PAIRING];
        struct link_attempt attempt[LINK_PAIRING * 2];
        positive attempt_next;
        struct waterlink_pair_seen seen;
        struct waterlink_admission admission;
        p8 name[WATERLINK_PAIR_NAME];
} link_nearby_state;

static link_nearby_state link_nearby;

static bool link_nearby_labels(p64 now)
{
        link_nearby.labels_ready = false;
        if (system_random_fill(link_nearby.host, sizeof link_nearby.host, 0) < 0)
                return false;
        for (positive at = 0; at < link_nearby.groups.count; at++)
        {
                struct waterlink_announce_group address_to group =
                        link_nearby.announce + at;

                if (system_random_fill(group->instance,
                                       sizeof group->instance, 0) < 0 ||
                    system_random_fill(group->nonce,
                                       sizeof group->nonce, 0) < 0)
                        return false;
                waterlink_tag(link_nearby.keys + at, group->nonce, group->tag);
                waterlink_who(link_nearby.keys + at, group->nonce,
                              link_self.me.public, group->who);
        }
        link_nearby.rotated = now;
        link_nearby.labels_ready = true;
        return true;
}

typedef struct
{
        p32 multiaddr;
        p32 address;
        b32 index;
} link_mreqn;

typedef struct
{
        char name[16];
        union {
                b32 index;
                socket_address_internet address;
                p8 pad[24];
        } value;
} link_ifreq;

/*
        Every interface that takes the membership, with its IPv4 address for
        the A record. A namespace in a test has no default route, so joining
        on "any" would fail there; one by one, by index, works everywhere.
*/
static fn link_nearby_interfaces(void)
{
        link_nearby.interfaces = 0;
        for (b32 index = 1; index < 64 && link_nearby.interfaces < LINK_INTERFACES;
             index++)
        {
                link_mreqn join = {network_order_32(WATERLINK_MDNS_GROUP), 0,
                                   index};
                link_ifreq request;
                p32 address = 0;

                bipolar joined = socket_option_set((b32)link_nearby.socket, 0,
                                                   35, // IP_ADD_MEMBERSHIP
                                                   address_of join, sizeof join);

                //      No such interface, or one without multicast; joined
                //      already (EADDRINUSE) is joined.
                if (joined < 0 && joined != -98)
                        continue;

                memory_zero(address_of request, sizeof request);
                request.value.index = index;
                if (system_control(link_nearby.socket, 0x8910, // SIOCGIFNAME
                                   address_of request) >= 0 &&
                    system_control(link_nearby.socket, 0x8915, // SIOCGIFADDR
                                   address_of request) >= 0)
                        address = network_order_32(request.value.address.host);

                link_nearby.interface[link_nearby.interfaces] = index;
                link_nearby.interface_address[link_nearby.interfaces] = address;
                link_nearby.interfaces++;
        }
}

static fn link_nearby_close(void)
{
        if (link_nearby.socket > 0)
                socket_close((b32)link_nearby.socket);
        link_nearby.socket = -1;
        link_nearby.interfaces = 0;
}

static fn link_nearby_open(void)
{
        b32 one = 1;
        b32 ttl = 255;
        socket_address_internet self;

        link_nearby.socket = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC |
                                                        SOCK_NONBLOCK,
                                        0);
        if (link_nearby.socket < 0)
                return;
        (void)socket_option_set((b32)link_nearby.socket, SOL_SOCKET,
                                SO_REUSEADDR, address_of one, sizeof one);
        (void)socket_option_set((b32)link_nearby.socket, SOL_SOCKET, 15, // REUSEPORT
                                address_of one, sizeof one);
        (void)socket_option_set((b32)link_nearby.socket, 0, 33, // MULTICAST_TTL
                                address_of ttl, sizeof ttl);
        (void)socket_option_set((b32)link_nearby.socket, 0, 12, // RECVTTL
                                address_of one, sizeof one);

        memory_zero(address_of self, sizeof self);
        self.family = AF_INET;
        self.port = network_order_16(WATERLINK_MDNS_PORT);
        if (socket_bind((b32)link_nearby.socket, address_of self,
                        sizeof self) < 0)
        {
                link_nearby_close();
                return;
        }
        link_nearby_interfaces();
}

/*
        The groups as the file says now, when it has changed: keys derived
        (cheaply -- the slow derivation happened at join), labels drawn, the
        socket opened for the first group and closed after the last.
*/
static fn link_nearby_reload(p64 now)
{
        file_facts facts;
        p64 changed = 0;

        //      Twice a second at most. Every save is a rename, so the inode
        //      says whether the file is the one already read.
        if (link_nearby.looked && now - link_nearby.looked < 500000)
                return;
        link_nearby.looked = now;
        if (file_look_at(LINK_GROUPS_PATH, address_of facts))
                changed = facts.inode * 31 + facts.size + 1;
        if (changed == link_nearby.groups_changed)
                return;
        link_nearby.groups_changed = changed;

        link_groups_load(address_of link_nearby.groups);
        for (positive at = 0; at < link_nearby.groups.count; at++)
                waterlink_group_keys_from(link_nearby.keys + at,
                                          link_nearby.groups.record[at].key,
                                          link_nearby.groups.record[at].namespace);
        link_machine_name(link_nearby.name);

        if (link_nearby.groups.count && link_nearby.socket < 0)
                link_nearby_open();
        if (!link_nearby.groups.count && link_nearby.socket >= 0)
                link_nearby_close();

        (void)link_nearby_labels(now);
        link_nearby.announced = 0;
        link_nearby.asked = 0;
        link_nearby.next_announce = now;
        link_nearby.next_ask = now;
}

static fn link_nearby_send(p8 address_to packet, positive length,
                           positive interface_at)
{
        link_mreqn out = {0, 0, link_nearby.interface[interface_at]};
        socket_address_internet to;

        memory_zero(address_of to, sizeof to);
        to.family = AF_INET;
        to.port = network_order_16(WATERLINK_MDNS_PORT);
        to.host = network_order_32(WATERLINK_MDNS_GROUP);
        (void)socket_option_set((b32)link_nearby.socket, 0, 32, // MULTICAST_IF
                                address_of out, sizeof out);
        (void)socket_send((b32)link_nearby.socket, packet, length,
                          MSG_NOSIGNAL, address_of to, sizeof to);
}

static fn link_nearby_announce(p32 ttl)
{
        p8 packet[WATERLINK_MDNS_MAX];

        for (positive at = 0; at < link_nearby.interfaces; at++)
        {
                positive length = waterlink_mdns_announce(
                        packet, sizeof packet, link_nearby.announce,
                        link_nearby.groups.count, link_nearby.host,
                        link_port(), link_nearby.interface_address[at], ttl, 0,
                        null, 0);

                if (length)
                        link_nearby_send(packet, length, at);
        }
}

static fn link_nearby_ask(void)
{
        p8 packet[64];
        positive length = waterlink_mdns_query(packet, sizeof packet);

        for (positive at = 0; at < link_nearby.interfaces; at++)
                link_nearby_send(packet, length, at);
}

// Goodbye, with TTL zero, when the listener stops.
static fn link_nearby_stop(void)
{
        if (link_nearby.socket >= 0 && link_nearby.groups.count)
                link_nearby_announce(0);
        link_nearby_close();
}

static struct link_pairing address_to link_pairing_free(void)
{
        for (positive at = 0; at < LINK_PAIRING; at++)
                if (!link_nearby.pairing[at].used)
                        return link_nearby.pairing + at;
        return null;
}

static bool link_attempted(p8 address_to address, p16 port, p64 now)
{
        for (positive at = 0; at < LINK_PAIRING * 2; at++)
        {
                struct link_attempt address_to attempt = link_nearby.attempt + at;

                if (attempt->at && now - attempt->at < LINK_PAIR_AGAIN &&
                    attempt->port == port &&
                    !memory_compare(attempt->address, address, 16))
                        return true;
        }
        {
                struct link_attempt address_to attempt =
                        link_nearby.attempt + link_nearby.attempt_next;

                memory_copy(attempt->address, address, 16);
                attempt->port = port;
                attempt->at = now ? now : 1;
                link_nearby.attempt_next =
                        (link_nearby.attempt_next + 1) % (LINK_PAIRING * 2);
        }
        return false;
}

// A member this machine has not met: send it a first message.
static fn link_pair_begin(positive group, p8 address_to address, p16 port,
                          p64 now)
{
        struct link_pairing address_to pairing;
        p8 datagram[WATERLINK_DATAGRAM];
        p8 ephemeral[32];

        if (link_attempted(address, port, now))
                return;
        pairing = link_pairing_free();
        if (!pairing)
                return;

        memory_zero(pairing, sizeof(address_to pairing));
        pairing->used = true;
        pairing->initiator = true;
        pairing->group = group;
        pairing->ours = link_index_new();
        if (!pairing->ours)
        {
                crypto_forget(pairing, sizeof(address_to pairing));
                return;
        }
        memory_copy(pairing->address, address, 16);
        pairing->port = port;
        pairing->started = now;

        if (system_random_fill(ephemeral, 32, 0) < 0)
        {
                crypto_forget(ephemeral, sizeof ephemeral);
                crypto_forget(pairing, sizeof(address_to pairing));
                return;
        }
        waterlink_pair_first(address_of pairing->noise,
                             link_nearby.keys[group].pair, ephemeral,
                             pairing->ours, datagram);
        crypto_forget(ephemeral, sizeof ephemeral);
        (void)link_send_to(datagram, WATERLINK_DATAGRAM, address, port);
}

/*
        One mDNS packet from the local link: a question about the service is
        answered, and an announcement is read for members.
*/
static fn link_nearby_heard(p8 address_to packet, positive length,
                            p8 address_to address, p16 source_port, p64 now)
{
        struct waterlink_found found;
        link_peers peers;
        bool peers_loaded = false;

        if (!waterlink_mdns_read(packet, length, address_of found))
                return;

        if (found.asked && link_nearby.groups.count)
        {
                if (source_port != WATERLINK_MDNS_PORT && found.question_length)
                {
                        //      A one-shot asker gets the answer back to
                        //      itself, its ID and question with it.
                        p8 reply[WATERLINK_MDNS_MAX];
                        positive reply_length = waterlink_mdns_announce(
                                reply, sizeof reply, link_nearby.announce,
                                link_nearby.groups.count, link_nearby.host,
                                link_port(),
                                link_nearby.interfaces
                                        ? link_nearby.interface_address[0]
                                        : 0,
                                10, found.id, found.question,
                                found.question_length);
                        socket_address_internet to;

                        memory_zero(address_of to, sizeof to);
                        to.family = AF_INET;
                        to.port = network_order_16(source_port);
                        to.host = network_order_32(network_load_32(address + 12));
                        if (reply_length)
                                (void)socket_send((b32)link_nearby.socket, reply,
                                                  reply_length, MSG_NOSIGNAL,
                                                  address_of to, sizeof to);
                }
                else if (now - link_nearby.last_answer > 1000000)
                {
                        link_nearby.last_answer = now;
                        link_nearby_announce(4500);
                }
        }

        for (positive at = 0; at < found.count; at++)
        {
                struct waterlink_found_instance address_to instance =
                        found.instance + at;
                bool ours = false;

                if (!instance->has_fields || !instance->has_port)
                        continue;

                //      Our own, back through the loop.
                for (positive group = 0; group < link_nearby.groups.count; group++)
                        if (!memory_compare(link_nearby.announce[group].nonce,
                                            instance->nonce,
                                            WATERLINK_NONCE_BYTES))
                                ours = true;
                if (ours)
                        continue;

                for (positive group = 0; group < link_nearby.groups.count; group++)
                {
                        struct waterlink_group_keys address_to keys =
                                link_nearby.keys + group;
                        p8 tag[WATERLINK_TAG_BYTES_SHORT];
                        bool known = false;

                        waterlink_tag(keys, instance->nonce, tag);
                        if (!crypto_same(tag, instance->tag,
                                         WATERLINK_TAG_BYTES_SHORT))
                                continue;

                        //      A member. Which one, of those this group
                        //      already paired?
                        if (!peers_loaded)
                        {
                                link_peers_load(address_of peers);
                                peers_loaded = true;
                        }
                        for (positive p = 0; p < peers.count && !known; p++)
                        {
                                p8 who[WATERLINK_TAG_BYTES_SHORT];

                                if (peers.peer[p].group != keys->mark)
                                        continue;
                                waterlink_who(keys, instance->nonce,
                                              peers.peer[p].key, who);
                                if (crypto_same(who, instance->who,
                                                WATERLINK_TAG_BYTES_SHORT))
                                {
                                        known = true;
                                        link_peer_moved(peers.peer[p].key,
                                                        address, instance->port);
                                }
                        }
                        if (!known)
                                link_pair_begin(group, address, instance->port,
                                                now);
                }
        }
}

/*
        The pairing datagrams, on the link's own socket. A first message is
        tried against each group's key, which is one AEAD check each and no
        curve at all; one that opens and was not seen before is answered.
*/
static fn link_pair_datagram(p8 address_to datagram, positive length,
                             p8 address_to address, p16 port, p64 now)
{
        struct waterlink_datagram head;

        if (length != WATERLINK_DATAGRAM || !link_nearby.groups.count)
                return;
        memory_copy(address_of head, datagram, 16);

        if (head.kind == WATERLINK_KIND_PAIR_1)
        {
                struct waterlink_noise noise;
                struct link_pairing address_to pairing;

                for (positive at = 16 + WATERLINK_PAIR_1_BYTES;
                     at < WATERLINK_DATAGRAM; at++)
                        if (datagram[at])
                                return;
                if (!waterlink_admit(address_of link_nearby.admission, address,
                                     now))
                        return;

                for (positive group = 0; group < link_nearby.groups.count; group++)
                {
                        p8 answer[WATERLINK_DATAGRAM];
                        p8 ephemeral[32];

                        if (!waterlink_pair_heard_first(
                                    address_of noise,
                                    link_nearby.keys[group].pair, datagram))
                                continue;
                        if (!waterlink_pair_fresh(address_of link_nearby.seen,
                                                  datagram + 16))
                        {
                                crypto_forget(address_of noise, sizeof noise);
                                return;
                        }
                        pairing = link_pairing_free();
                        if (!pairing)
                        {
                                crypto_forget(address_of noise, sizeof noise);
                                return;
                        }

                        memory_zero(pairing, sizeof(address_to pairing));
                        pairing->used = true;
                        pairing->group = group;
                        pairing->theirs = head.receiver;
                        pairing->ours = link_index_new();
                        if (!pairing->ours)
                        {
                                crypto_forget(pairing,
                                              sizeof(address_to pairing));
                                crypto_forget(address_of noise, sizeof noise);
                                return;
                        }
                        memory_copy(pairing->address, address, 16);
                        pairing->port = port;
                        pairing->started = now;
                        pairing->noise = noise;
                        crypto_forget(address_of noise, sizeof noise);

                        if (system_random_fill(ephemeral, 32, 0) < 0)
                        {
                                crypto_forget(ephemeral, sizeof ephemeral);
                                crypto_forget(pairing,
                                              sizeof(address_to pairing));
                                return;
                        }
                        if (!waterlink_pair_second(address_of pairing->noise,
                                                   address_of link_self.me,
                                                   ephemeral, link_nearby.name,
                                                   pairing->theirs, answer))
                        {
                                crypto_forget(ephemeral, sizeof ephemeral);
                                crypto_forget(pairing,
                                              sizeof(address_to pairing));
                                return;
                        }
                        crypto_forget(ephemeral, sizeof ephemeral);
                        //      Our index rides in the counter field, the one
                        //      the initiator does not use.
                        memory_copy(answer + 8, address_of pairing->ours, 4);
                        (void)link_send_to(answer, WATERLINK_DATAGRAM, address,
                                           port);
                        return;
                }
                crypto_forget(address_of noise, sizeof noise);
                return;
        }

        for (positive at = 0; at < LINK_PAIRING; at++)
        {
                struct link_pairing address_to pairing = link_nearby.pairing + at;
                p8 key[32];
                p8 name[WATERLINK_PAIR_NAME];

                if (!pairing->used || pairing->ours != head.receiver)
                        continue;

                if (head.kind == WATERLINK_KIND_PAIR_2 && pairing->initiator)
                {
                        p8 third[WATERLINK_DATAGRAM];
                        p32 theirs;

                        memory_copy(address_of theirs, datagram + 8, 4);
                        if (!waterlink_pair_heard_second(address_of pairing->noise,
                                                         datagram, key, name))
                                return;
                        if (!waterlink_pair_third(address_of pairing->noise,
                                                  address_of link_self.me,
                                                  link_nearby.name, theirs,
                                                  third))
                        {
                                crypto_forget(pairing,
                                              sizeof(address_to pairing));
                                return;
                        }
                        (void)link_send_to(third, WATERLINK_DATAGRAM, address,
                                           port);
                        (void)link_pair_keep(link_nearby.groups.record +
                                                     pairing->group,
                                             link_nearby.keys + pairing->group,
                                             key, name, pairing->address,
                                             pairing->port);
                }
                else if (head.kind == WATERLINK_KIND_PAIR_3 && !pairing->initiator)
                {
                        if (!waterlink_pair_heard_third(address_of pairing->noise,
                                                        address_of link_self.me,
                                                        datagram, key, name))
                                return;
                        (void)link_pair_keep(link_nearby.groups.record +
                                                     pairing->group,
                                             link_nearby.keys + pairing->group,
                                             key, name, address, port);
                }
                else
                        return;

                crypto_forget(pairing, sizeof(address_to pairing));
                return;
        }
}

/*
        The listener's turn: reload the groups if they changed, rotate, send
        what is due, time out pairings. Answers when it next wants a turn.
*/
static p64 link_nearby_tick(p64 now)
{
        p64 wake = now + 1000000;

        link_nearby_reload(now);
        if (link_nearby.socket < 0 || !link_nearby.groups.count)
                return wake;
        if (!link_nearby.labels_ready && !link_nearby_labels(now))
                return wake;

        //      Interfaces come and go, and an address comes late -- a lease
        //      that arrives after the listener started is the ordinary case
        //      on a machine that boots straight into it. Every five seconds
        //      the memberships are asked for again, and an address that is
        //      new starts the quick announcements over.
        if (now - link_nearby.interfaces_looked >= 5000000)
        {
                p32 before[LINK_INTERFACES];
                positive had = link_nearby.interfaces;
                bool changed;

                memory_copy(before, link_nearby.interface_address,
                            sizeof before);
                link_nearby.interfaces_looked = now;
                link_nearby_interfaces();
                changed = had != link_nearby.interfaces ||
                          memory_compare(before, link_nearby.interface_address,
                                         had * sizeof(p32));
                if (changed)
                {
                        link_nearby.announced = 0;
                        link_nearby.asked = 0;
                        link_nearby.next_announce = now;
                        link_nearby.next_ask = now;
                }
        }

        if (now - link_nearby.rotated >= LINK_ROTATE_EVERY)
        {
                link_nearby_announce(0);
                if (!link_nearby_labels(now))
                        return wake;
                link_nearby.announced = 0;
        }

        //      Three quick announcements and questions at the start, as
        //      RFC 6762 asks of a responder, then the steady pace.
        if (now >= link_nearby.next_announce)
        {
                link_nearby_announce(4500);
                link_nearby.announced++;
                link_nearby.next_announce =
                        now + (link_nearby.announced < 3 ? 1000000
                                                         : LINK_ANNOUNCE_EVERY);
        }
        if (now >= link_nearby.next_ask)
        {
                link_nearby_ask();
                link_nearby.asked++;
                link_nearby.next_ask =
                        now + (link_nearby.asked < 3 ? 1000000 : LINK_ASK_EVERY);
        }

        for (positive at = 0; at < LINK_PAIRING; at++)
                if (link_nearby.pairing[at].used &&
                    now - link_nearby.pairing[at].started > LINK_PAIR_WAIT)
                        crypto_forget(link_nearby.pairing + at,
                                      sizeof(link_nearby.pairing[0]));

        if (link_nearby.next_announce < wake)
                wake = link_nearby.next_announce;
        if (link_nearby.next_ask < wake)
                wake = link_nearby.next_ask;
        return wake;
}

// Read what is waiting on the mDNS socket, believing only TTL 255.
static fn link_nearby_receive(p64 now)
{
        for (positive turn = 0; turn < 64 && link_nearby.socket >= 0; turn++)
        {
                p8 packet[WATERLINK_MDNS_MAX + 1];
                socket_address_internet from;
                link_iovec part = {packet, sizeof packet};
                p64 control[8];
                link_message message;
                bipolar got;
                b32 ttl = -1;
                p8 address[16];

                memory_zero(address_of message, sizeof message);
                message.name = address_of from;
                message.name_length = sizeof from;
                message.parts = address_of part;
                message.part_count = 1;
                message.control = control;
                message.control_length = sizeof control;
                got = system_call_3(syscall(recvmsg), (positive)link_nearby.socket,
                                    (positive)address_of message, MSG_DONTWAIT);
                if (got < 0)
                        return;
                if ((positive)got > WATERLINK_MDNS_MAX)
                        continue;

                //      cmsghdr: length, level, type, then the TTL as an int.
                for (positive at = 0;
                     at + 16 <= message.control_length && at + 16 <= sizeof control;)
                {
                        p64 cmsg_length;
                        b32 level, type;

                        memory_copy(address_of cmsg_length, (p8 address_to)control + at, 8);
                        memory_copy(address_of level, (p8 address_to)control + at + 8, 4);
                        memory_copy(address_of type, (p8 address_to)control + at + 12, 4);
                        if (cmsg_length < 16 || at + cmsg_length > sizeof control)
                                break;
                        if (level == 0 && type == 2 && cmsg_length >= 20)
                                memory_copy(address_of ttl, (p8 address_to)control + at + 16, 4);
                        at += (cmsg_length + 7) & ~7ull;
                }
                if (ttl != 255)
                        continue;

                link_address_v4(address, network_order_32(from.host));
                link_nearby_heard(packet, (positive)got, address,
                                  network_order_16(from.port), now);
        }
}

#endif // WATERLINK_NEARBY_INCLUDED
