/*
        Waterlink as a program: the listener, the sessions, and the far end of
        `moonwater link shell` and `moonwater link run`.

        One process, one UDP socket, one poll. The core (link.c), the seal
        (seal.c) and the handshake (handshake.c) are transforms; this is the
        part that owns a socket, a clock, the keys and the children, and it is
        kept to that. Userspace on purpose -- the datagram path is where the
        kernel would be faster, and it is also where a mistake costs the
        machine, so it waits until the protocol has been used.

        WHAT A SESSION CARRIES

        A client opens a session per command, asks for one thing on it (a
        terminal or a command), and the session ends when that thing does.
        The streams are link keys, and each key has one job:

                request   client to machine, durable: what is wanted
                answer    machine to client, durable: yes, or why not
                input     client to machine, durable: keystrokes (urgent,
                          so each leaves alone at once) or standard input
                size      client to machine, replaceable: the window's size,
                          so a drag of the corner sends the last size only
                signal    client to machine, durable: a signal for the
                          command, when there is no terminal to type ^C at
                credit    machine to client, replaceable: how much input the
                          machine will take, which is how a command that
                          reads slowly slows the sender down
                output    machine to client, durable: the terminal's bytes,
                          or standard output, ending in the exit status
                error     machine to client, durable: standard error

        Every payload starts with one byte naming what it is -- data, end,
        exit -- so a stream ends in order with what it carried.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_SERVICE_INCLUDED
#define WATERLINK_SERVICE_INCLUDED

#include "link.c"
#include "seal.c"
#include "handshake.c"

/*      "WL", two letters as sixteen bits: 0x574c. Unassigned, above the
        registered ranges people firewall by habit, and easy to remember from
        the name. */
#define LINK_PORT 22348

#define LINK_SWITCH_PATH "/root/link"
#define LINK_KEY_PATH "/root/link.key"
#define LINK_PEERS_PATH "/root/link.peers"
#define LINK_PEERS_NEXT "/root/link.peers.next"
#define LINK_PORT_PATH "/root/link.port"
#define LINK_LOCK_PATH HOST_STATE "/link.lock"
#define LINK_STATE_PATH HOST_STATE "/link.state"
#define LINK_STATE_NEXT HOST_STATE "/link.state.next"

#define LINK_PEERS_MAX 64
#define LINK_SESSIONS 16
#define LINK_SESSIONS_A_PEER 8
#define LINK_INPUT_ROOM (256 * 1024)
#define LINK_REQUEST_MAX 1024

#define LINK_KEEPALIVE 10000000ull   // microseconds of silence, then a datagram
#define LINK_DEAD 45000000ull        // of hearing nothing, then the session is gone
#define LINK_GRACE 20000000ull       // old receive keys after a rekey
#define LINK_ATTEMPT 1000000ull      // between initiations
#define LINK_ATTEMPTS 6

#define LINK_KEY_REQUEST 1
#define LINK_KEY_ANSWER 2
#define LINK_KEY_INPUT 3
#define LINK_KEY_SIZE 4
#define LINK_KEY_SIGNAL 5
#define LINK_KEY_CREDIT 6
#define LINK_KEY_OUTPUT 7
#define LINK_KEY_ERROR 8

#define LINK_DATA 'D'
#define LINK_END 'E'
#define LINK_EXIT 'X'

#define LINK_ASK_SHELL 'S'
#define LINK_ASK_RUN 'R'
#define LINK_ASK_PUSH 'P'
#define LINK_ASK_PULL 'G'
#define LINK_ASK_LOG 'L'

#define LINK_KIND_NONE 0
#define LINK_KIND_SHELL 1
#define LINK_KIND_RUN 2
#define LINK_KIND_PUSH 3
#define LINK_KIND_PULL 4
#define LINK_KIND_LOG 5

#define LINK_EXIT_BUSY 3
#define LINK_FAILED 255

typedef struct
{
        string_address name;
        p32 bit;
} link_grant;

static const link_grant link_grants[] = {
    {"verbs", WATERLINK_MAY_VERBS},     {"run", WATERLINK_MAY_RUN},
    {"shell", WATERLINK_MAY_SHELL},     {"screen", WATERLINK_MAY_SCREEN},
    {"files", WATERLINK_MAY_FILES},     {"log", WATERLINK_MAY_LOG},
    {"channels", WATERLINK_MAY_CHANNELS},
};

static p64 link_now(void)
{
        return system_clock_ns(1) / 1000;
}

// All digits and nothing else, below a million; -1 otherwise.
static bipolar link_decimal(string_address text)
{
        bipolar value = 0;

        if (!text || !text[0])
                return -1;
        for (positive at = 0; text[at]; at++)
        {
                if (text[at] < '0' || text[at] > '9' || at > 6)
                        return -1;
                value = value * 10 + (text[at] - '0');
        }
        return value;
}

static fn link_append(p8 address_to text, positive address_to used,
                      positive room, string_address piece)
{
        positive length = string_length(piece);

        if (address_to used + length + 1 > room)
                return;
        memory_copy(text + address_to used, piece, length);
        address_to used += length;
        text[address_to used] = 0;
}

static fn link_append_number(p8 address_to text, positive address_to used,
                             positive room, p64 value)
{
        p8 digits[24];

        digits[positive_into(digits, (positive)value)] = 0;
        link_append(text, used, room, (string_address)digits);
}

// Store --------------------------------------------------------------

static const char link_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// A key as WireGuard writes one: 44 characters of base64.
static fn link_key_text(p8 address_to key, p8 address_to text)
{
        positive out = 0;

        for (positive at = 0; at < 33; at += 3)
        {
                p32 group = (p32)key[at] << 16;

                if (at + 1 < 32)
                        group |= (p32)key[at + 1] << 8;
                if (at + 2 < 32)
                        group |= key[at + 2];

                text[out++] = (p8)link_alphabet[(group >> 18) & 63];
                text[out++] = (p8)link_alphabet[(group >> 12) & 63];
                text[out++] = at + 1 < 32 ? (p8)link_alphabet[(group >> 6) & 63]
                                          : '=';
                text[out++] = at + 2 < 32 ? (p8)link_alphabet[group & 63] : '=';
        }
        text[44] = 0;
}

static bool link_key_parse(string_address text, p8 address_to key)
{
        p8 word[48];

        if (string_length(text) != 44 || text[43] != '=')
                return false;

        for (positive at = 0; at < 43; at++)
        {
                positive value = 64;

                for (positive look = 0; look < 64; look++)
                        if (link_alphabet[look] == text[at])
                                value = look;
                if (value == 64)
                        return false;
                word[at] = (p8)value;
        }

        for (positive at = 0, out = 0; at < 44; at += 4)
        {
                p32 group = (p32)word[at] << 18 | (p32)word[at + 1] << 12 |
                            (at + 2 < 43 ? (p32)word[at + 2] << 6 : 0) |
                            (at + 3 < 43 ? word[at + 3] : 0);

                key[out++] = (p8)(group >> 16);
                if (out < 32)
                        key[out++] = (p8)(group >> 8);
                if (out < 32)
                        key[out++] = (p8)group;
        }

        //      The last character carries two bits the key does not use;
        //      a text with them set is some other text.
        return !(word[42] & 3);
}

/*
        A peer's name is typed by the person pairing it and printed back in
        every status, so it is held to letters, digits, dot, dash and
        underscore, starting with a letter or digit: nothing a terminal would
        act on, nothing a shell word would split.
*/
static bool link_name_good(string_address name)
{
        positive length = string_length(name);

        if (!length || length >= WATERLINK_NAME_MAX)
                return false;

        for (positive at = 0; at < length; at++)
        {
                p8 c = (p8)name[at];
                bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9');

                if (!letter && (!at || (c != '.' && c != '-' && c != '_')))
                        return false;
        }
        return true;
}

static bipolar link_read_exact(string_address path, p8 address_to into,
                               positive room, positive address_to got)
{
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_NOFOLLOW | O_CLOEXEC);
        positive have = 0;

        if (handle < 0)
                return handle;

        while (have < room)
        {
                bipolar read = system_read_once(handle, into + have,
                                                room - have);

                if (read == -4)
                        continue;
                if (read <= 0)
                        break;
                have += (positive)read;
        }

        system_close(handle);
        address_to got = have;
        return 0;
}

/* Authorization databases are not ordinary state text. Refuse a partial,
   oversized, linked, non-root-owned or publicly writable set rather than
   interpreting the valid-looking prefix of a replaced file. */
static bipolar link_read_private_records(string_address path,
                                         p8 address_to into, positive room,
                                         positive record,
                                         positive address_to got)
{
        file_facts facts;
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_NOFOLLOW | O_CLOEXEC);
        positive have = 0;

        address_to got = 0;
        if (handle < 0)
                return handle;
        if (!file_look(handle, (string_address)"", AT_EMPTY_PATH,
                       address_of facts) ||
            (facts.mode & MODE_FORMAT) != MODE_FILE || facts.owner != 0 ||
            (facts.mode & 077) || facts.size > room || facts.size % record)
        {
                system_close(handle);
                return -EPERM;
        }

        while (have < (positive)facts.size)
        {
                bipolar read = system_read_once(handle, into + have,
                                                (positive)facts.size - have);

                if (read == -4)
                        continue;
                if (read <= 0)
                        break;
                have += (positive)read;
        }
        system_close(handle);
        if (have != (positive)facts.size)
                return -EIO;
        address_to got = have;
        return 0;
}

/*
        The machine's own key, made on first use.

        Made under a name nobody else could have chosen and linked into place,
        so two first uses at once cannot leave half a key, and a name planted
        at /root/link.key -- a link to somewhere, or a file somebody could read
        -- is refused rather than written through or trusted.
*/
static bipolar link_secret(p8 address_to secret, bool make)
{
        file_facts facts;
        bipolar handle = system_open_at(AT_FDCWD, LINK_KEY_PATH,
                                        FILE_READ | O_NOFOLLOW | O_CLOEXEC);

        if (handle == -ENOENT && make)
        {
                p8 fresh[32];
                p8 name[64] = "/root/.link.key.";
                p8 tail[8];
                positive used = string_length((string_address)name);
                bipolar made;

                if (system_random_fill(fresh, 32, 0) < 0 ||
                    system_random_fill(tail, 8, 0) < 0)
                {
                        crypto_forget(fresh, sizeof fresh);
                        crypto_forget(tail, sizeof tail);
                        return -EIO;
                }
                for (positive at = 0; at < 8; at++)
                {
                        name[used++] = (p8)link_alphabet[tail[at] >> 4 & 15];
                        name[used++] = (p8)link_alphabet[tail[at] & 15];
                }
                name[used] = 0;

                made = system_open_at_mode(AT_FDCWD, name,
                                           FILE_WRITE | FILE_EXCLUSIVE |
                                                   O_NOFOLLOW | O_CLOEXEC,
                                           0600);
                if (made < 0)
                {
                        crypto_forget(fresh, sizeof fresh);
                        crypto_forget(tail, sizeof tail);
                        return made;
                }
                if (system_write_all((positive)made, fresh, 32) != 32 ||
                    system_call_1(syscall(fsync), (positive)made) < 0)
                {
                        system_close(made);
                        system_remove_at(AT_FDCWD, name, 0);
                        crypto_forget(fresh, sizeof fresh);
                        crypto_forget(tail, sizeof tail);
                        return -EIO;
                }
                system_close(made);
                crypto_forget(fresh, sizeof fresh);
                crypto_forget(tail, sizeof tail);

                //      linkat does not replace: whoever got there first wins,
                //      and both read what won.
                made = system_call_5(syscall(linkat), (positive)(bipolar)AT_FDCWD,
                                     (positive)name,
                                     (positive)(bipolar)AT_FDCWD,
                                     (positive)LINK_KEY_PATH, 0);
                system_remove_at(AT_FDCWD, name, 0);
                if (made < 0 && made != -ERROR_EXISTS)
                        return made;

                handle = system_open_at(AT_FDCWD, LINK_KEY_PATH,
                                        FILE_READ | O_NOFOLLOW | O_CLOEXEC);
        }

        if (handle < 0)
                return handle;

        if (!file_look(handle, (string_address) "", AT_EMPTY_PATH,
                       address_of facts) ||
            (facts.mode & MODE_FORMAT) != MODE_FILE || facts.size != 32 ||
            (facts.mode & 077) || facts.owner != 0)
        {
                system_close(handle);
                return -EPERM;
        }

        {
                positive have = 0;

                while (have < 32)
                {
                        bipolar read = system_read_once(handle, secret + have,
                                                        32 - have);

                        if (read == -4)
                                continue;
                        if (read <= 0)
                                break;
                        have += (positive)read;
                }
                system_close(handle);
                return have == 32 ? 0 : -EIO;
        }
}

static bipolar link_identity(struct waterlink_identity address_to me, bool make)
{
        p8 secret[32];
        bipolar got = link_secret(secret, make);

        if (got < 0)
                return got;
        waterlink_identity_from(me, secret);
        crypto_forget(secret, sizeof secret);
        return 0;
}

typedef struct
{
        struct waterlink_peer peer[LINK_PEERS_MAX];
        positive count;
} link_peers;

static fn link_peers_load(link_peers address_to peers)
{
        positive got = 0;

        memory_zero(peers, sizeof(address_to peers));
        if (link_read_private_records(LINK_PEERS_PATH,
                                      (p8 address_to)peers->peer,
                                      sizeof(peers->peer),
                                      sizeof(struct waterlink_peer),
                                      address_of got) < 0)
                return;

        peers->count = got / sizeof(struct waterlink_peer);

        //      A record with a name that could not have been paired is dropped
        //      rather than printed.
        for (positive at = 0; at < peers->count; at++)
        {
                peers->peer[at].name[WATERLINK_NAME_MAX - 1] = 0;
                if (!link_name_good(peers->peer[at].name))
                {
                        peers->peer[at] = peers->peer[--peers->count];
                        at--;
                }
        }
}

static bipolar link_peers_save(link_peers address_to peers)
{
        bipolar failed = host_write_file(LINK_PEERS_NEXT,
                                         (p8 address_to)peers->peer,
                                         peers->count *
                                                 sizeof(struct waterlink_peer),
                                         0600, true);

        if (failed < 0)
                return failed;
        return system_rename_at(AT_FDCWD, LINK_PEERS_NEXT, AT_FDCWD,
                                LINK_PEERS_PATH, 0);
}

static struct waterlink_peer address_to link_peer_named(link_peers address_to peers,
                                                         string_address name)
{
        for (positive at = 0; at < peers->count; at++)
                if (string_equals(peers->peer[at].name, name))
                        return peers->peer + at;
        return null;
}

static struct waterlink_peer address_to link_peer_keyed(link_peers address_to peers,
                                                         p8 address_to key)
{
        for (positive at = 0; at < peers->count; at++)
                if (crypto_same(peers->peer[at].key, key, 32))
                        return peers->peer + at;
        return null;
}

static bool link_wanted(void)
{
        p8 word[16];

        host_read_text(LINK_SWITCH_PATH, word, sizeof word);
        return string_equals((string_address)word, "on");
}

static p16 link_port(void)
{
        p8 word[16];
        bipolar port;

        host_read_text(LINK_PORT_PATH, word, sizeof word);
        port = link_decimal((string_address)word);
        return port > 0 && port < 65536 ? (p16)port : LINK_PORT;
}

// Addresses ------------------------------------------------------------

/*
        Every address is held as IPv6, IPv4 mapped when it is IPv4, which is
        what a dual-stack socket reports and what a peer record keeps.
*/
static fn link_address_v4(p8 address_to into, p32 host)
{
        memory_zero(into, 10);
        into[10] = 0xff;
        into[11] = 0xff;
        network_store_32(into + 12, host);
}

static bool link_address_mapped(p8 address_to address)
{
        static const p8 prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

        return !memory_compare(address, prefix, 12);
}

static bool link_hex_group(string_address text, positive length,
                           p16 address_to into)
{
        p32 value = 0;

        if (!length || length > 4)
                return false;
        for (positive at = 0; at < length; at++)
        {
                p8 c = (p8)text[at];
                p32 digit = c >= '0' && c <= '9'   ? c - '0'
                            : c >= 'a' && c <= 'f' ? c - 'a' + 10
                            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                   : 16;

                if (digit == 16)
                        return false;
                value = value << 4 | digit;
        }
        address_to into = (p16)value;
        return true;
}

// IPv6 text, with one :: at most and no embedded IPv4.
static bool link_parse_v6(string_address text, positive length,
                          p8 address_to into)
{
        p16 head[8];
        p16 tail[8];
        positive heads = 0;
        positive tails = 0;
        bool gap = false;
        positive at = 0;

        if (length >= 2 && text[0] == ':' && text[1] == ':')
        {
                gap = true;
                at = 2;
        }

        while (at < length)
        {
                positive start = at;
                p16 group;

                while (at < length && text[at] != ':')
                        at++;
                if (!link_hex_group(text + start, at - start, address_of group))
                        return false;
                if (gap)
                {
                        if (tails >= 8)
                                return false;
                        tail[tails++] = group;
                }
                else
                {
                        if (heads >= 8)
                                return false;
                        head[heads++] = group;
                }
                if (at < length)
                {
                        at++;
                        if (at < length && text[at] == ':')
                        {
                                if (gap)
                                        return false;
                                gap = true;
                                at++;
                        }
                        else if (at == length)
                                return false;
                }
        }

        if (heads + tails > (gap ? 7u : 8u) || (!gap && heads != 8))
                return false;

        memory_zero(into, 16);
        for (positive i = 0; i < heads; i++)
        {
                into[2 * i] = (p8)(head[i] >> 8);
                into[2 * i + 1] = (p8)head[i];
        }
        for (positive i = 0; i < tails; i++)
        {
                positive slot = 8 - tails + i;

                into[2 * slot] = (p8)(tail[i] >> 8);
                into[2 * slot + 1] = (p8)tail[i];
        }
        return true;
}

/*
        HOST, HOST:PORT, [V6] or [V6]:PORT. A name is looked up once, now, over
        IPv4: an address is a cache of where a key last was, and the next
        datagram from the peer corrects it.
*/
static bool link_parse_place(string_address text, p8 address_to address,
                             p16 address_to port)
{
        p8 host[256];
        positive length = string_length(text);
        string_address colon = null;
        bipolar numeric;

        address_to port = LINK_PORT;

        if (text[0] == '[')
        {
                string_address close = string_first_of(text, ']');

                if (!close)
                        return false;
                if (close[1] == ':')
                        colon = close + 1;
                else if (close[1])
                        return false;
                if (!link_parse_v6(text + 1, (positive)(close - text - 1),
                                   address))
                        return false;
        }
        else
        {
                string_address first = string_first_of(text, ':');

                if (first && string_first_of(first + 1, ':'))
                        return link_parse_v6(text, length, address);
                colon = first;
                length = colon ? (positive)(colon - text) : length;
                if (!length || length >= sizeof host)
                        return false;
                memory_copy(host, text, length);
                host[length] = 0;

                numeric = string_to_host((string_address)host);
                if (numeric >= 0)
                        link_address_v4(address, (p32)numeric);
                else
                {
                        p32 found = 0;

                        if (dns_resolve_any((string_address) "/etc/resolv.conf",
                                            (string_address)host,
                                            address_of found, 3) != DNS_OK)
                                return false;
                        link_address_v4(address, found);
                }
        }

        if (colon)
        {
                bipolar value = link_decimal(colon + 1);

                if (value <= 0 || value > 65535)
                        return false;
                address_to port = (p16)value;
        }
        return true;
}

// Digits and colons only: what this writes can be printed to a terminal.
static fn link_place_text(p8 address_to address, p16 port, p8 address_to text)
{
        positive used = 0;

        if (link_address_mapped(address))
        {
                for (positive at = 12; at < 16; at++)
                {
                        used += positive_into(text + used, address[at]);
                        text[used++] = at < 15 ? '.' : ':';
                }
        }
        else
        {
                static const char digits[] = "0123456789abcdef";

                text[used++] = '[';
                for (positive at = 0; at < 16; at += 2)
                {
                        p32 group = (p32)address[at] << 8 | address[at + 1];
                        bool started = false;

                        for (bipolar shift = 12; shift >= 0; shift -= 4)
                        {
                                p32 digit = group >> shift & 15;

                                if (digit || started || !shift)
                                {
                                        text[used++] = (p8)digits[digit];
                                        started = true;
                                }
                        }
                        if (at < 14)
                                text[used++] = ':';
                }
                text[used++] = ']';
                text[used++] = ':';
        }
        used += positive_into(text + used, port);
        text[used] = 0;
}

static fn link_socket_address(socket_address_internet6 address_to into,
                              p8 address_to address, p16 port)
{
        memory_zero(into, sizeof(address_to into));
        into->family = AF_INET6;
        into->port = network_order_16(port);
        memory_copy(into->host, address, 16);
}

// The lock --------------------------------------------------------------

/*
        The listener holds a write lock on /run/moonwater/link.lock for as long
        as it runs. A record lock and not flock, because the kernel will say
        who holds a record lock: `link off` asks, opens the holder as a pidfd,
        asks again, and only then signals -- so a pid that ended and was reused
        between the two is never the one signalled.
*/
typedef struct
{
        b16 type;
        b16 whence;
        b32 padding;
        bipolar start;
        bipolar length;
        b32 pid;
        b32 padding2;
} link_record_lock;

#define LINK_F_GETLK 5
#define LINK_F_SETLK 6
#define LINK_F_WRLCK 1
#define LINK_F_UNLCK 2

static bipolar link_lock_open(void)
{
        host_state_ready();
        return system_open_at_mode(AT_FDCWD, LINK_LOCK_PATH,
                                   FILE_READ_WRITE | FILE_CREATE | O_NOFOLLOW |
                                           O_CLOEXEC,
                                   0600);
}

static bipolar link_lock_owner(void)
{
        link_record_lock lock = {LINK_F_WRLCK, 0, 0, 0, 0, 0, 0};
        bipolar handle = link_lock_open();
        bipolar asked;

        if (handle < 0)
                return 0;
        asked = system_call_3(syscall(fcntl), (positive)handle, LINK_F_GETLK,
                              (positive)address_of lock);
        system_close(handle);
        if (asked < 0 || lock.type == LINK_F_UNLCK)
                return 0;
        return lock.pid;
}

static bipolar link_lock_take(void)
{
        link_record_lock lock = {LINK_F_WRLCK, 0, 0, 0, 0, 0, 0};
        bipolar handle = link_lock_open();

        if (handle < 0)
                return handle;
        if (system_call_3(syscall(fcntl), (positive)handle, LINK_F_SETLK,
                          (positive)address_of lock) < 0)
        {
                system_close(handle);
                return -EAGAIN;
        }
        return handle;
}

// A signal for whoever holds the lock, and never for a pid reused since.
static bool link_lock_signal(b32 signal)
{
        bipolar owner = link_lock_owner();
        bipolar handle;
        bool sent;

        if (owner <= 0)
                return false;
        handle = system_call_2(syscall(pidfd_open), (positive)owner, 0);
        if (handle < 0)
                return false;
        sent = link_lock_owner() == owner &&
               system_call_4(syscall(pidfd_send_signal), (positive)handle,
                             (positive)signal, 0, 0) >= 0;
        system_close(handle);
        return sent;
}

// Sessions ---------------------------------------------------------------

struct link_keys {
        crypto_aesgcm_key send;
        crypto_aesgcm_key receive;
        struct waterlink_replay replay;
        p64 counter;
        p64 made;
        p32 ours;
        p32 theirs;
        bool live;
};

struct link_session {
        bool used;
        bool finished;
        p8 kind;
        p8 name[WATERLINK_NAME_MAX];
        p8 peer[32];
        p32 may;
        p64 conversation;
        p8 address[16];
        p16 port;
        struct link_keys now;
        struct link_keys next;   // made by a rekey, waiting to be confirmed
        struct link_keys before; // what the last rekey replaced
        p64 opened;
        p64 keyings; // how many handshakes this session has made
        p64 heard;
        p64 spoke;
        struct waterlink_link address_to link;

        //      The machine's end: the command and its streams.
        bipolar pid;
        bipolar pidfd;
        bipolar terminal;
        bipolar input;
        bipolar output;
        bipolar error;
        bool exited;
        b32 status;
        p64 exited_at;
        bool output_read;
        bool error_read;
        bool error_ended;
        bool exit_sent;
        bool failed;
        p8 push_name[LINK_REQUEST_MAX + 1];
        p8 push_part[LINK_REQUEST_MAX + 32];
        p8 address_to pending;
        positive pending_at;
        positive pending_length;
        bool pending_end;
        p64 consumed;
        p64 credited;
};

typedef struct
{
        bipolar socket;
        bool server;
        bool gso;
        bool v4; // no IPv6 here: the socket is AF_INET
        struct waterlink_identity me;
        struct waterlink_admission admission;
        struct link_session session[LINK_SESSIONS];
        p8 stamp_key[LINK_PEERS_MAX][32];
        p8 stamp[LINK_PEERS_MAX][WATERLINK_STAMP_BYTES];
        positive stamps;
        p64 state_written;
        bool state_dirty;
        //      A run of full datagrams for one place, sent as segments.
        p8 batch[64 * WATERLINK_DATAGRAM];
        positive batched;
        p8 batch_address[16];
        p16 batch_port;
} link_service;

static link_service link_self;

static p32 link_index_new(void)
{
        for (positive attempt = 0; attempt < 128; attempt++)
        {
                p32 index = 0;
                bool taken = false;

                if (system_random_fill(address_of index, sizeof index, 0) < 0)
                        return 0;
                if (!index)
                        continue;
                for (positive at = 0; at < LINK_SESSIONS; at++)
                {
                        struct link_session address_to s = link_self.session + at;

                        if (s->used && (s->now.ours == index ||
                                        s->next.ours == index ||
                                        s->before.ours == index))
                                taken = true;
                }
                if (!taken)
                        return index;
        }
        return 0;
}

static bool link_session_open(struct link_session address_to s)
{
        bipolar mapped = system_call_6(syscall(mmap), 0,
                                       sizeof(struct waterlink_link), 3, 0x22,
                                       (positive)(bipolar)-1, 0);

        if (mapped < 0 && mapped > -4096)
                return false;

        memory_zero(s, sizeof(address_to s));
        s->link = (struct waterlink_link address_to)mapped;
        waterlink_link_reset(s->link);
        s->used = true;
        s->pid = 0;
        s->pidfd = s->terminal = s->input = s->output = s->error = -1;
        s->opened = s->heard = s->spoke = link_now();
        return true;
}

static fn link_session_close(struct link_session address_to s)
{
        /* An interrupted push does not publish or retain its private staging
           inode. */
        if (s->kind == LINK_KIND_PUSH && s->push_part[0])
                system_remove_at(AT_FDCWD, s->push_part, 0);
        if (s->pidfd >= 0)
        {
                (void)system_call_4(syscall(pidfd_send_signal),
                                    (positive)s->pidfd, 1, 0, 0);
                system_close(s->pidfd);
        }
        if (s->pid > 0)
                (void)system_call_2(syscall(kill), (positive)-s->pid, 1);
        if (s->terminal >= 0)
                system_close(s->terminal);
        if (s->input >= 0)
                system_close(s->input);
        if (s->output >= 0)
                system_close(s->output);
        if (s->error >= 0)
                system_close(s->error);
        if (s->pending)
                system_call_2(syscall(munmap), (positive)s->pending,
                              LINK_INPUT_ROOM);
        if (s->link)
                system_call_2(syscall(munmap), (positive)s->link,
                              sizeof(struct waterlink_link));
        crypto_forget(s, sizeof(address_to s));
        link_self.state_dirty = true;
}

static fn link_keys_install(struct link_keys address_to keys,
                            p8 address_to send, p8 address_to receive,
                            p32 ours, p32 theirs)
{
        struct link_session address_to owner = null;

        for (positive at = 0; at < LINK_SESSIONS; at++)
                if ((p8 address_to)keys >= (p8 address_to)(link_self.session + at) &&
                    (p8 address_to)keys < (p8 address_to)(link_self.session + at + 1))
                        owner = link_self.session + at;
        if (owner)
                owner->keyings++;
        memory_zero(keys, sizeof(address_to keys));
        crypto_aesgcm_prepare(address_of keys->send, send);
        crypto_aesgcm_prepare(address_of keys->receive, receive);
        keys->ours = ours;
        keys->theirs = theirs;
        keys->made = link_now();
        keys->live = true;
}

// Sending ------------------------------------------------------------------

/*
        Where a datagram goes, in the socket's own family: an IPv4 address is
        mapped on a dual-stack socket and plain on an IPv4 one.
*/
static positive link_destination(socket_address_internet6 address_to into,
                                 p8 address_to address, p16 port)
{
        if (link_self.v4)
        {
                socket_address_internet address_to v4 =
                        (socket_address_internet address_to)into;

                memory_zero(into, sizeof(address_to into));
                v4->family = AF_INET;
                v4->port = network_order_16(port);
                v4->host = network_order_32(network_load_32(address + 12));
                return sizeof(socket_address_internet);
        }
        link_socket_address(into, address, port);
        return sizeof(address_to into);
}

static bipolar link_send_to(p8 address_to bytes, positive length,
                            p8 address_to address, p16 port)
{
        socket_address_internet6 to;
        positive size = link_destination(address_of to, address, port);

        if (link_self.v4 && !link_address_mapped(address))
                return -97; // EAFNOSUPPORT: no IPv6 here
        return socket_send((b32)link_self.socket, bytes, length, MSG_NOSIGNAL,
                           address_of to, size);
}

/*
        A run of full datagrams to one place in one call, cut by the kernel:
        UDP_SEGMENT, which is what the fixed size is for (waterlink.c). A
        kernel or a device that refuses it once is not asked again.
*/
typedef struct
{
        address_any base;
        positive length;
} link_iovec;

typedef struct
{
        address_any name;
        b32 name_length;
        b32 padding;
        link_iovec address_to parts;
        positive part_count;
        address_any control;
        positive control_length;
        b32 flags;
        b32 padding2;
} link_message;

static fn link_batch_flush(void)
{
        positive count = link_self.batched;

        link_self.batched = 0;
        if (!count)
                return;

        if (count > 1 && link_self.gso)
        {
                socket_address_internet6 to;
                link_iovec part = {link_self.batch, count * WATERLINK_DATAGRAM};
                p64 control[3];
                link_message message;
                bipolar sent;

                positive size = link_destination(address_of to,
                                                 link_self.batch_address,
                                                 link_self.batch_port);
                memory_zero(control, sizeof control);
                control[0] = 18;                        // cmsg_len
                ((b32 address_to)control)[2] = 17;      // SOL_UDP
                ((b32 address_to)control)[3] = 103;     // UDP_SEGMENT
                ((p16 address_to)control)[8] = WATERLINK_DATAGRAM;

                memory_zero(address_of message, sizeof message);
                message.name = address_of to;
                message.name_length = (b32)size;
                message.parts = address_of part;
                message.part_count = 1;
                message.control = control;
                message.control_length = 24;

                sent = system_call_3(syscall(sendmsg), (positive)link_self.socket,
                                     (positive)address_of message,
                                     MSG_NOSIGNAL);
                if (sent >= 0 || sent == -EAGAIN)
                        return;
                link_self.gso = false;
        }

        for (positive at = 0; at < count; at++)
                (void)link_send_to(link_self.batch + at * WATERLINK_DATAGRAM,
                                   WATERLINK_DATAGRAM, link_self.batch_address,
                                   link_self.batch_port);
}

/*
        Everything a session's link will send now: each datagram numbered,
        sealed and sent, the full ones that need not go alone into a run.
*/
static fn link_session_flush(struct link_session address_to s, p64 now)
{
        for (positive guard = 0; guard < 256; guard++)
        {
                p8 address_to datagram;
                struct waterlink_datagram head;
                bool alone = false;
                positive used;
                p8 single[WATERLINK_DATAGRAM];

                if (link_self.batched == 64 ||
                    (link_self.batched &&
                     (memory_compare(link_self.batch_address, s->address, 16) ||
                      link_self.batch_port != s->port)))
                        link_batch_flush();

                datagram = link_self.batch + link_self.batched *
                                                     WATERLINK_DATAGRAM;
                used = waterlink_fill(s->link, datagram + 16, now,
                                      address_of alone);
                if (!used)
                        break;

                head.kind = WATERLINK_KIND_CARRY;
                head.receiver = s->now.theirs;
                head.counter = s->now.counter++;
                memory_copy(datagram, address_of head, 16);
                s->spoke = now;

                if (!s->link->carried)
                {
                        positive length;

                        memory_copy(single, datagram, 16 + used);
                        length = waterlink_seal_short(address_of s->now.send,
                                                      single, used);
                        (void)link_send_to(single, length, s->address, s->port);
                        continue;
                }

                waterlink_seal(address_of s->now.send, datagram, used);
                if (alone)
                {
                        (void)link_send_to(datagram, WATERLINK_DATAGRAM,
                                           s->address, s->port);
                        continue;
                }

                memory_copy(link_self.batch_address, s->address, 16);
                link_self.batch_port = s->port;
                link_self.batched++;
        }
        link_batch_flush();
}

// A sealed datagram of nothing: a keepalive, or the end.
static fn link_session_say(struct link_session address_to s, p32 kind)
{
        p8 datagram[64];
        struct waterlink_datagram head = {kind, s->now.theirs,
                                          s->now.counter++};
        positive length;

        memory_copy(datagram, address_of head, 16);
        length = waterlink_seal_short(address_of s->now.send, datagram, 0);
        (void)link_send_to(datagram, length, s->address, s->port);
        s->spoke = link_now();
}

static bool link_post(struct link_session address_to s, p64 key, p16 flags,
                      p8 type, p8 address_to data, positive length)
{
        p8 payload[WATERLINK_FRAME_MAX];

        if (length > WATERLINK_FRAME_MAX - 1)
                return false;
        payload[0] = type;
        if (length)
                memory_copy(payload + 1, data, length);
        return waterlink_post(s->link, key, 0, flags, 0, 0, payload,
                              (p16)(length + 1), link_now());
}

// Room for a frame and one more, so an ending always has a slot to take.
static bool link_room(struct link_session address_to s)
{
        p32 at = s->link->free;

        return at != WATERLINK_NONE && s->link->slot[at].next != WATERLINK_NONE;
}

/*
        How many frames may be posted now, keeping one slot back so an ending
        always has somewhere to go: a read is sized to this, so a busy stream
        costs one system call for a run of frames and not one a frame.
*/
#define LINK_READ_FRAMES 48
#define LINK_CHUNK (WATERLINK_FRAME_MAX - 1)

static positive link_room_frames(struct link_session address_to s)
{
        positive count = 0;

        for (p32 at = s->link->free;
             at != WATERLINK_NONE && count <= LINK_READ_FRAMES;
             at = s->link->slot[at].next)
                count++;
        return count ? count - 1 : 0;
}

static p8 link_read_buffer[LINK_READ_FRAMES * LINK_CHUNK];

// The machine's end ---------------------------------------------------------

static fn link_state_write(p64 now);

/*
        TERM as the client sent it, if it is a plain name; a terminal type
        is a file name to curses, so anything else becomes xterm.
*/
static fn link_term_word(p8 address_to from, positive length,
                         p8 address_to into)
{
        bool good = length && length < 32;

        for (positive at = 0; good && at < length; at++)
        {
                p8 c = from[at];

                good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                       c == '+' || c == '_';
        }

        if (!good)
        {
                string_copy((string_address)into, "xterm");
                return;
        }
        memory_copy(into, from, length);
        into[length] = 0;
}

static DEAD_END fn link_child_exec(string_address command, p8 address_to term)
{
        p8 term_line[48] = "TERM=";
        p8 path_line[512] = "PATH=/bin:/sbin:/usr/bin:/usr/sbin";
        string_address path = file_environment((string_address) "PATH");
        string_address environment[8];
        string_address words[4];
        positive count = 0;
        positive blocked = 0;

        (void)system_signal_mask(2, address_of blocked, null, 8);
        for (b32 signal = 1; signal < 32; signal++)
        {
                p64 action[4] = {0, 0, 0, 0};

                (void)system_call_4(syscall(rt_sigaction), (positive)signal,
                                    (positive)action, 0, 8);
        }

        if (path && string_length(path) < sizeof path_line - 6)
        {
                string_copy((string_address)path_line, "PATH=");
                string_copy((string_address)path_line + 5, path);
        }
        string_copy((string_address)term_line + 5, (string_address)term);

        environment[count++] = (string_address)path_line;
        environment[count++] = (string_address)term_line;
        environment[count++] = (string_address) "HOME=/root";
        environment[count++] = (string_address) "USER=root";
        environment[count++] = (string_address) "LOGNAME=root";
        environment[count++] = (string_address) "SHELL=/bin/sh";
        environment[count] = null;

        (void)system_call_1(syscall(chdir), (positive)(string_address) "/root");

        words[0] = (string_address) "sh";
        if (command)
        {
                words[1] = (string_address) "-c";
                words[2] = command;
                words[3] = null;
        }
        else
        {
                words[1] = (string_address) "-i";
                words[2] = null;
        }

        (void)shell_exec_file((string_address) "/proc/self/exe", words,
                              command ? 3 : 2, environment);
        system_call_1(syscall(exit), 127);
        __builtin_unreachable();
}

static bool link_nonblocking(bipolar handle)
{
        return system_call_3(syscall(fcntl), (positive)handle, 4,
                             O_NONBLOCK | FILE_READ_WRITE) >= 0;
}

static bool link_start_shell(struct link_session address_to s,
                             p8 address_to request, positive length)
{
        b32 master = -1;
        b32 slave = -1;
        winsize size = {24, 80, 0, 0};
        p8 term[32];
        bipolar child;

        if (length >= 4)
        {
                size.rows = (p16)(request[0] | request[1] << 8);
                size.columns = (p16)(request[2] | request[3] << 8);
        }
        link_term_word(request + 4, length > 4 ? length - 4 : 0, term);

        if (process_pty_open(address_of master, address_of slave, true) < 0)
                return false;
        system_control(slave, TIOCSWINSZ, address_of size);

        child = system_fork();
        if (child < 0)
        {
                system_close(master);
                system_close(slave);
                return false;
        }
        if (!child)
        {
                if (process_pty_child_setup(master, slave, -1, -1) < 0)
                        system_call_1(syscall(exit), 126);
                link_child_exec(null, term);
        }

        system_close(slave);
        s->pid = child;
        s->pidfd = system_call_2(syscall(pidfd_open), (positive)child, 0);
        s->terminal = master;
        s->kind = LINK_KIND_SHELL;
        s->error_read = true;
        s->error_ended = true;
        return true;
}

static bool link_start_run(struct link_session address_to s,
                           p8 address_to request, positive length)
{
        b32 in[2], out[2], err[2];
        p8 command[LINK_REQUEST_MAX + 1];
        bipolar child;

        if (!length || length > LINK_REQUEST_MAX)
                return false;
        memory_copy(command, request, length);
        command[length] = 0;
        if (string_length((string_address)command) != length)
                return false;

        if (system_pipe(in, O_CLOEXEC) < 0)
                return false;
        if (system_pipe(out, O_CLOEXEC) < 0)
        {
                system_close(in[0]);
                system_close(in[1]);
                return false;
        }
        if (system_pipe(err, O_CLOEXEC) < 0)
        {
                system_close(in[0]);
                system_close(in[1]);
                system_close(out[0]);
                system_close(out[1]);
                return false;
        }

        child = system_fork();
        if (!child)
        {
                (void)system_call(syscall(setsid));
                if (system_descriptor_install(in[0], 0) < 0 ||
                    system_descriptor_install(out[1], 1) < 0 ||
                    system_descriptor_install(err[1], 2) < 0)
                        system_call_1(syscall(exit), 126);
                link_child_exec((string_address)command,
                                (p8 address_to) "dumb");
        }

        system_close(in[0]);
        system_close(out[1]);
        system_close(err[1]);
        if (child < 0)
        {
                system_close(in[1]);
                system_close(out[0]);
                system_close(err[0]);
                return false;
        }

        link_nonblocking(in[1]);
        link_nonblocking(out[0]);
        link_nonblocking(err[0]);
        s->pid = child;
        s->pidfd = system_call_2(syscall(pidfd_open), (positive)child, 0);
        s->input = in[1];
        s->output = out[0];
        s->error = err[0];
        s->kind = LINK_KIND_RUN;
        return true;
}

/*
        push, pull and log: the same streams as run, with a file or the
        kernel log where the command's pipes were and no process behind them.
        A pushed file is written where it is named but never through a link
        standing at that name, and is only there under its name once it is
        whole; a pulled file is read as it is.
*/
static bipolar link_part_open(string_address target, positive length,
                              p8 address_to part, positive room, p32 mode)
{
        if (length + 28 > room)
                return -ERROR_NAME_TOO_LONG;

        memory_copy(part, target, length);
        memory_copy(part + length, ".link-part.", 11);
        for (positive attempt = 0; attempt < 8; attempt++)
        {
                p64 random;
                bipolar handle;

                if (system_random_fill(address_of random, sizeof random, 0) < 0)
                        return -EIO;
                for (positive at = 0; at < 16; at++)
                        part[length + 11 + at] =
                                (p8)link_alphabet[(random >> (at * 4)) & 15];
                part[length + 27] = 0;
                handle = system_open_at_mode(
                        AT_FDCWD, part,
                        FILE_WRITE | FILE_EXCLUSIVE | O_NOFOLLOW | O_CLOEXEC,
                        mode);
                if (handle >= 0 || handle != -ERROR_EXISTS)
                        return handle;
        }
        return -ERROR_EXISTS;
}

static bool link_start_file(struct link_session address_to s, p8 ask,
                            p8 address_to request, positive length)
{
        p8 path[LINK_REQUEST_MAX + 32];
        p32 mode = 0644;
        bipolar handle;

        if (ask == LINK_ASK_LOG)
        {
                handle = system_open_at(AT_FDCWD, "/dev/kmsg",
                                        FILE_READ | O_NONBLOCK | O_CLOEXEC);
                if (handle < 0)
                        return false;
                s->output = handle;
                s->kind = LINK_KIND_LOG;
                s->error_read = true;
                s->error_ended = true;
                return true;
        }

        if (ask == LINK_ASK_PUSH)
        {
                if (length < 4)
                        return false;
                memory_copy(address_of mode, request, 4);
                mode &= 07777;
                request += 4;
                length -= 4;
        }
        if (!length || length > LINK_REQUEST_MAX)
                return false;
        memory_copy(path, request, length);
        path[length] = 0;
        if (string_length((string_address)path) != length)
                return false;

        if (ask == LINK_ASK_PULL)
        {
                handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);
                if (handle < 0)
                        return false;
                s->output = handle;
                s->kind = LINK_KIND_PULL;
                s->error_read = true;
                return true;
        }

        /* A fresh exclusive name prevents both collisions between concurrent
           transfers and a planted predictable name from denying every push.
           It remains beside the target so the final rename is atomic. */
        handle = link_part_open((string_address)path, length, path,
                                sizeof path, mode);
        if (handle < 0)
                return false;
        (void)system_call_2(syscall(fchmod), (positive)handle, mode);
        memory_copy(s->push_name, path, length);
        s->push_name[length] = 0;
        string_copy((string_address)s->push_part, (string_address)path);
        s->input = handle;
        s->kind = LINK_KIND_PUSH;
        s->output_read = true;
        s->error_read = true;
        return true;
}

static fn link_refuse(struct link_session address_to s, string_address why)
{
        (void)link_post(s, LINK_KEY_ANSWER,
                        WATERLINK_FRAME_DURABLE | WATERLINK_FRAME_LAST, 'N',
                        (p8 address_to)why, string_length(why));
        //      Ends once the far side has the answer.
        s->exit_sent = true;
        s->kind = LINK_KIND_NONE;
}

static fn link_request(struct link_session address_to s, p8 address_to payload,
                       positive length)
{
        link_peers peers;
        struct waterlink_peer address_to peer;
        p32 may = 0;
        bool started;
        p8 why[96];

        if (s->kind != LINK_KIND_NONE || length < 1)
                return;

        //      Grants as they are now, not as they were at the handshake.
        link_peers_load(address_of peers);
        peer = link_peer_keyed(address_of peers, s->peer);
        if (peer)
                may = peer->may;
        s->may = may;

        if (payload[0] == LINK_ASK_SHELL && !(may & WATERLINK_MAY_SHELL))
        {
                string_copy((string_address)why, "shell is not granted to ");
                string_copy((string_address)why + string_length((string_address)why),
                            (string_address)s->name);
                link_refuse(s, (string_address)why);
                return;
        }
        if (payload[0] == LINK_ASK_RUN && !(may & WATERLINK_MAY_RUN))
        {
                string_copy((string_address)why, "run is not granted to ");
                string_copy((string_address)why + string_length((string_address)why),
                            (string_address)s->name);
                link_refuse(s, (string_address)why);
                return;
        }

        if ((payload[0] == LINK_ASK_PUSH || payload[0] == LINK_ASK_PULL) &&
            !(may & WATERLINK_MAY_FILES))
        {
                string_copy((string_address)why, "files is not granted to ");
                string_copy((string_address)why + string_length((string_address)why),
                            (string_address)s->name);
                link_refuse(s, (string_address)why);
                return;
        }
        if (payload[0] == LINK_ASK_LOG && !(may & WATERLINK_MAY_LOG))
        {
                string_copy((string_address)why, "log is not granted to ");
                string_copy((string_address)why + string_length((string_address)why),
                            (string_address)s->name);
                link_refuse(s, (string_address)why);
                return;
        }

        if (payload[0] == LINK_ASK_SHELL)
                started = link_start_shell(s, payload + 1, length - 1);
        else if (payload[0] == LINK_ASK_RUN)
                started = link_start_run(s, payload + 1, length - 1);
        else if (payload[0] == LINK_ASK_PUSH || payload[0] == LINK_ASK_PULL ||
                 payload[0] == LINK_ASK_LOG)
        {
                started = link_start_file(s, payload[0], payload + 1,
                                          length - 1);
                if (!started)
                {
                        link_refuse(s, payload[0] == LINK_ASK_LOG
                                               ? "the kernel log cannot be read"
                                               : "that file cannot be opened there");
                        return;
                }
        }
        else
        {
                link_refuse(s, "that is not something this machine offers");
                return;
        }

        if (!started)
        {
                link_refuse(s, "the command could not be started");
                return;
        }

        {
                bipolar mapped = system_call_6(syscall(mmap), 0,
                                               LINK_INPUT_ROOM, 3, 0x22,
                                               (positive)(bipolar)-1, 0);

                s->pending = mapped < 0 && mapped > -4096
                                     ? null
                                     : (p8 address_to)mapped;
        }

        (void)link_post(s, LINK_KEY_ANSWER,
                        WATERLINK_FRAME_DURABLE | WATERLINK_FRAME_LAST, 'O',
                        null, 0);
        {
                p64 credit = LINK_INPUT_ROOM;

                (void)link_post(s, LINK_KEY_CREDIT, WATERLINK_FRAME_REPLACEABLE,
                                'C', (p8 address_to)address_of credit, 8);
                s->credited = credit;
        }
        link_self.state_dirty = true;
}

static fn link_input(struct link_session address_to s, p8 type,
                     p8 address_to data, positive length)
{
        if (type == LINK_END)
        {
                s->pending_end = true;
                return;
        }
        if (type != LINK_DATA || !s->pending)
                return;

        //      Credit is what keeps this from happening; a client that sends
        //      past it is broken, and loses what did not fit.
        if (s->pending_at + s->pending_length + length > LINK_INPUT_ROOM)
        {
                if (s->pending_at)
                {
                        memory_copy(s->pending, s->pending + s->pending_at,
                                    s->pending_length);
                        s->pending_at = 0;
                }
                if (s->pending_length + length > LINK_INPUT_ROOM)
                        length = LINK_INPUT_ROOM - s->pending_length;
        }
        memory_copy(s->pending + s->pending_at + s->pending_length, data,
                    length);
        s->pending_length += length;
}

/*
        A frame arrives at the machine's end. The payload's first byte says
        what it is; the key says which stream.
*/
static fn link_server_hear(address_any context,
                           struct waterlink_frame address_to head,
                           p8 address_to payload)
{
        struct link_session address_to s = (struct link_session address_to)context;
        positive length = head->length;

        if (!length)
                return;

        switch (head->key)
        {
        case LINK_KEY_REQUEST:
                link_request(s, payload, length);
                break;
        case LINK_KEY_INPUT:
                link_input(s, payload[0], payload + 1, length - 1);
                break;
        case LINK_KEY_SIZE:
                if (length >= 5 && s->terminal >= 0)
                {
                        winsize size = {(p16)(payload[1] | payload[2] << 8),
                                        (p16)(payload[3] | payload[4] << 8), 0,
                                        0};

                        system_control(s->terminal, TIOCSWINSZ,
                                       address_of size);
                }
                break;
        case LINK_KEY_SIGNAL:
                if (length >= 2 && s->pid > 0 && !s->exited &&
                    (payload[1] == 1 || payload[1] == 2 || payload[1] == 3 ||
                     payload[1] == 9 || payload[1] == 15))
                        (void)system_call_2(syscall(kill), (positive)-s->pid,
                                            payload[1]);
                break;
        default:
                break;
        }
}

static fn link_push_done(struct link_session address_to s, p64 now)
{
        //      The part file becomes the name only whole.
        if (!s->failed)
        {
                file_facts opened;
                file_facts named;
                p8 whole[LINK_REQUEST_MAX + 16];
                positive length = string_length((string_address)s->push_name);
                bool owned;

                memory_copy(whole, s->push_name, length + 1);
                owned = file_look(s->input, (string_address)"", AT_EMPTY_PATH,
                                  address_of opened) &&
                        file_look(AT_FDCWD, (string_address)s->push_part,
                                  AT_SYMLINK_NOFOLLOW, address_of named) &&
                        file_same_identity(address_of opened,
                                           address_of named);
                if (!owned)
                {
                        s->failed = true;
                        /* The name no longer belongs to this transfer. */
                        s->push_part[0] = 0;
                }
                else if (system_rename_at(AT_FDCWD, s->push_part, AT_FDCWD,
                                          whole, 0) < 0)
                        s->failed = true;
                else
                        s->push_part[0] = 0;
        }
        s->exited = true;
        s->exited_at = now;
        s->status = s->failed ? 1 : 0;
}

/*
        The kernel log a record at a time, as /dev/kmsg hands it out, written
        as dmesg writes it: the seconds since boot in brackets, then the text.
        The record's continuation lines -- the dictionary -- are left out.
*/
static fn link_log_read(struct link_session address_to s)
{
        p8 record[8192];

        while (link_room(s))
        {
                bipolar got = system_read_once(s->output, record,
                                               sizeof record - 1);
                p8 line[WATERLINK_FRAME_MAX];
                positive used = 0;
                positive at = 0;
                p64 micro = 0;
                positive field = 0;

                if (got == -32)
                        continue; // EPIPE: records were overwritten
                if (got <= 0)
                        break;

                while (at < (positive)got && record[at] != ';')
                {
                        if (record[at] == ',')
                                field++;
                        else if (field == 2 && record[at] >= '0' &&
                                 record[at] <= '9')
                                micro = micro * 10 + (record[at] - '0');
                        at++;
                }
                at++;

                line[used++] = '[';
                {
                        p8 digits[24];
                        positive count = positive_into(digits,
                                                       (positive)(micro / 1000000));
                        p64 fraction = micro % 1000000;

                        for (positive pad = count; pad < 5; pad++)
                                line[used++] = ' ';
                        memory_copy(line + used, digits, count);
                        used += count;
                        line[used++] = '.';
                        for (bipolar place = 5; place >= 0; place--)
                        {
                                p64 tenth = 1;

                                for (bipolar k = 0; k < place; k++)
                                        tenth *= 10;
                                line[used++] = (p8)('0' + fraction / tenth % 10);
                        }
                }
                line[used++] = ']';
                line[used++] = ' ';

                while (at < (positive)got && record[at] != '\n' &&
                       used < sizeof line - 2)
                        line[used++] = record[at++];
                line[used++] = '\n';

                (void)link_post(s, LINK_KEY_OUTPUT, WATERLINK_FRAME_DURABLE,
                                LINK_DATA, line, used);
        }
}

// The command's streams, moved: input to it, output from it, and its end.
static fn link_session_streams(struct link_session address_to s, p64 now)
{
        bipolar input = s->terminal >= 0 ? s->terminal : s->input;

        //      Input, as far as the command will take it.
        while (input >= 0 && s->pending_length)
        {
                bipolar wrote = system_write_once(input,
                                                  s->pending + s->pending_at,
                                                  s->pending_length);

                if (wrote < 0 && wrote != -EAGAIN && wrote != -4 &&
                    s->kind == LINK_KIND_PUSH)
                {
                        //      A disk that refuses: the rest is dropped and
                        //      the push fails, rather than reading forever.
                        s->failed = true;
                        s->consumed += s->pending_length;
                        s->pending_length = 0;
                        s->pending_at = 0;
                        break;
                }
                if (wrote <= 0)
                        break;
                s->pending_at += (positive)wrote;
                s->pending_length -= (positive)wrote;
                s->consumed += (positive)wrote;
                if (!s->pending_length)
                        s->pending_at = 0;
        }
        if (s->pending_end && !s->pending_length && s->input >= 0)
        {
                if (s->kind == LINK_KIND_PUSH &&
                    system_call_1(syscall(fsync), (positive)s->input) < 0)
                        s->failed = true;
                if (s->kind == LINK_KIND_PUSH)
                        link_push_done(s, now);
                system_close(s->input);
                s->input = -1;
        }
        if (s->consumed + LINK_INPUT_ROOM >= s->credited + LINK_INPUT_ROOM / 4)
        {
                p64 credit = s->consumed + LINK_INPUT_ROOM;

                if (link_post(s, LINK_KEY_CREDIT, WATERLINK_FRAME_REPLACEABLE,
                              'C', (p8 address_to)address_of credit, 8))
                        s->credited = credit;
        }

        if (s->kind == LINK_KIND_LOG)
        {
                link_log_read(s);
                return;
        }

        //      Output, while the link has room for it.
        for (positive turn = 0; turn < 2; turn++)
        {
                bipolar address_to handle =
                        turn ? address_of s->error
                             : (s->terminal >= 0 ? address_of s->terminal
                                                 : address_of s->output);
                bool address_to done = turn ? address_of s->error_read
                                            : address_of s->output_read;

                while (address_to handle >= 0 && !address_to done &&
                       link_room(s))
                {
                        positive frames = link_room_frames(s);
                        bipolar got = system_read_once(address_to handle,
                                                       link_read_buffer,
                                                       frames * LINK_CHUNK);

                        if (got == -EAGAIN || got == -4)
                                break;
                        if (got <= 0)
                        {
                                //      A terminal whose last holder closed it
                                //      reads as EIO: that is its end.
                                address_to done = true;
                                if (s->kind == LINK_KIND_PULL)
                                {
                                        s->failed = got < 0;
                                        s->exited = true;
                                        s->exited_at = now;
                                        s->status = s->failed ? 1 : 0;
                                }
                                break;
                        }
                        for (positive at = 0; at < (positive)got;
                             at += LINK_CHUNK)
                                (void)link_post(s, turn ? LINK_KEY_ERROR
                                                        : LINK_KEY_OUTPUT,
                                                WATERLINK_FRAME_DURABLE,
                                                LINK_DATA,
                                                link_read_buffer + at,
                                                (positive)got - at < LINK_CHUNK
                                                        ? (positive)got - at
                                                        : LINK_CHUNK);
                }
        }

        //      The command's end, once: its status, after everything it said.
        if (s->pidfd >= 0 && !s->exited)
        {
                p8 information[128];

                memory_zero(information, sizeof information);
                //      siginfo: code at 8, the pid at 16 (zero while it
                //      runs, under WNOHANG), the status at 24.
                if (system_call_5(syscall(waitid), 3, (positive)s->pidfd,
                                  (positive)information, 4 | 1, 0) >= 0 &&
                    ((b32 address_to)information)[4])
                {
                        b32 code = ((b32 address_to)information)[2];
                        b32 value = ((b32 address_to)information)[6];

                        s->exited = true;
                        s->exited_at = now;
                        s->status = code == 1 ? value : 128 + value;
                }
        }

        //      A terminal can stay open behind a command that left something
        //      running in the background; a moment after the command ends, the
        //      session ends with it, as ssh's does.
        if (s->exited && !s->output_read && now - s->exited_at > 300000)
                s->output_read = true;
        if (s->exited && !s->error_read && now - s->exited_at > 300000)
                s->error_read = true;

        if (s->exited && s->output_read && s->error_read && !s->error_ended &&
            link_room(s))
                s->error_ended = link_post(s, LINK_KEY_ERROR,
                                           WATERLINK_FRAME_DURABLE |
                                                   WATERLINK_FRAME_LAST,
                                           LINK_END, null, 0);

        if (s->exited && s->output_read && s->error_ended && !s->exit_sent &&
            link_room(s))
                s->exit_sent = link_post(s, LINK_KEY_OUTPUT,
                                         WATERLINK_FRAME_DURABLE |
                                                 WATERLINK_FRAME_LAST,
                                         LINK_EXIT,
                                         (p8 address_to)address_of s->status,
                                         4);
}

// The handshake, at the machine's end -----------------------------------------

static bool link_stamp_fresh(p8 address_to key, p8 address_to stamp)
{
        positive at;

        for (at = 0; at < link_self.stamps; at++)
                if (crypto_same(link_self.stamp_key[at], key, 32))
                        break;

        if (at < link_self.stamps)
        {
                if (!waterlink_stamp_newer(stamp, link_self.stamp[at]))
                        return false;
        }
        else
        {
                if (link_self.stamps < LINK_PEERS_MAX)
                        at = link_self.stamps++;
                else
                        at = 0;
                memory_copy(link_self.stamp_key[at], key, 32);
        }
        memory_copy(link_self.stamp[at], stamp, WATERLINK_STAMP_BYTES);
        return true;
}

static fn link_server_initiation(p8 address_to datagram, positive length,
                                 p8 address_to address, p16 port, p64 now)
{
        struct waterlink_noise noise;
        p8 who[32];
        p8 hello[WATERLINK_HELLO_BYTES];
        p8 ephemeral[32];
        p8 answer[WATERLINK_DATAGRAM];
        p8 send[16], receive[16];
        link_peers peers;
        struct waterlink_peer address_to peer;
        struct link_session address_to s = null;
        positive of_peer = 0;
        p64 conversation;
        p32 theirs;
        p32 ours;

        if (!waterlink_gate_passes(address_of link_self.me, datagram, length) ||
            !waterlink_admit(address_of link_self.admission, address, now))
                return;
        if (!waterlink_accept(address_of noise, address_of link_self.me,
                              datagram, who, hello))
        {
                crypto_forget(address_of noise, sizeof noise);
                return;
        }

        link_peers_load(address_of peers);
        peer = link_peer_keyed(address_of peers, who);
        if (!peer || !link_stamp_fresh(who, hello))
        {
                crypto_forget(address_of noise, sizeof noise);
                return;
        }

        memory_copy(address_of conversation, hello + WATERLINK_STAMP_BYTES, 8);
        memory_copy(address_of theirs, hello + WATERLINK_STAMP_BYTES + 8, 4);

        for (positive at = 0; at < LINK_SESSIONS; at++)
        {
                struct link_session address_to look = link_self.session + at;

                if (!look->used || !crypto_same(look->peer, who, 32))
                        continue;
                of_peer++;
                if (look->conversation == conversation)
                        s = look;
        }

        /* Draw everything the answer needs before reserving a session: an
           entropy outage must not let initiations fill the session table. */
        ours = link_index_new();
        if (!ours || system_random_fill(ephemeral, 32, 0) < 0)
        {
                crypto_forget(ephemeral, sizeof ephemeral);
                crypto_forget(address_of noise, sizeof noise);
                return;
        }

        if (!s)
        {
                if (of_peer >= LINK_SESSIONS_A_PEER)
                        return;
                for (positive at = 0; at < LINK_SESSIONS && !s; at++)
                        if (!link_self.session[at].used)
                                s = link_self.session + at;
                if (!s || !link_session_open(s))
                        return;
                memory_copy(s->peer, who, 32);
                memory_copy(s->name, peer->name, WATERLINK_NAME_MAX);
                s->may = peer->may;
                s->conversation = conversation;
        }

        if (!waterlink_respond(address_of noise, ephemeral, theirs, ours,
                               answer))
        {
                crypto_forget(ephemeral, sizeof ephemeral);
                crypto_forget(address_of noise, sizeof noise);
                return;
        }
        waterlink_split(address_of noise, false, send, receive);
        crypto_forget(ephemeral, sizeof ephemeral);

        //      A session keyed again sends under the old keys until the
        //      initiator has shown it holds the new ones.
        if (s->now.live)
                link_keys_install(address_of s->next, send, receive, ours,
                                  theirs);
        else
                link_keys_install(address_of s->now, send, receive, ours,
                                  theirs);
        crypto_forget(send, sizeof send);
        crypto_forget(receive, sizeof receive);

        memory_copy(s->address, address, 16);
        s->port = port;
        s->heard = now;
        (void)link_send_to(answer, WATERLINK_DATAGRAM, address, port);
        link_self.state_dirty = true;
}

// Receiving, at either end ----------------------------------------------------

static struct link_keys address_to link_keys_for(p32 index,
                                                 struct link_session address_to address_to found)
{
        for (positive at = 0; at < LINK_SESSIONS; at++)
        {
                struct link_session address_to s = link_self.session + at;

                if (!s->used)
                        continue;
                address_to found = s;
                if (s->now.live && s->now.ours == index)
                        return address_of s->now;
                if (s->next.live && s->next.ours == index)
                        return address_of s->next;
                if (s->before.live && s->before.ours == index)
                        return address_of s->before;
        }
        return null;
}

static fn link_session_end(struct link_session address_to s, bool tell)
{
        if (tell && s->now.live)
        {
                link_session_say(s, WATERLINK_KIND_CLOSE);
                link_session_say(s, WATERLINK_KIND_CLOSE);
        }
        s->finished = true;
}

/*
        One sealed datagram at either end: the index finds the session and the
        keys, the tag and the replay window decide whether it counts, and only
        then does it move the session -- to the address it came from, which is
        all roaming is, and to the keys it proved, which is how a rekey is
        confirmed.
*/
static fn link_note_seen(struct link_session address_to s, p64 wall);

static bool link_carried(p8 address_to datagram, positive length,
                         p8 address_to address, p16 port, p64 now,
                         waterlink_sink sink)
{
        struct waterlink_datagram head;
        struct link_session address_to s = null;
        struct link_keys address_to keys;

        memory_copy(address_of head, datagram, 16);
        keys = link_keys_for(head.receiver, address_of s);
        if (!keys || !waterlink_open_length(address_of keys->receive, datagram,
                                            length))
                return false;
        if (!waterlink_replay_new(address_of keys->replay, head.counter))
                return false;
        if (waterlink_session_spent(now - keys->made, keys->counter))
                return false;
        if (keys == address_of s->before && now - s->now.made > LINK_GRACE)
                return false;

        if (keys == address_of s->next)
        {
                s->before = s->now;
                s->now = s->next;
                s->next.live = false;
        }

        if (memory_compare(s->address, address, 16) || s->port != port)
                link_self.state_dirty = true;
        memory_copy(s->address, address, 16);
        s->port = port;
        s->heard = now;
        if (link_self.server)
        {
                link_note_seen(s, system_clock_ns(0) / 1000000000ull);
                link_self.state_dirty = true;
        }

        if (head.kind == WATERLINK_KIND_CLOSE)
        {
                s->finished = true;
                return true;
        }
        if (head.kind != WATERLINK_KIND_CARRY)
                return false;

        return waterlink_deliver_at(s->link, datagram + 16, length - 32, now,
                                    sink, s);
}

static bipolar link_receive(p8 address_to datagram, p8 address_to address,
                            p16 address_to port)
{
        socket_address_internet6 from;
        b32 size = sizeof from;
        bipolar got = socket_receive((b32)link_self.socket, datagram,
                                     WATERLINK_DATAGRAM + 16, MSG_DONTWAIT,
                                     address_of from, address_of size);

        if (got < 0)
                return got;
        if (from.family == AF_INET6)
        {
                memory_copy(address, from.host, 16);
                address_to port = network_order_16(from.port);
        }
        else
        {
                socket_address_internet address_to v4 =
                        (socket_address_internet address_to)address_of from;

                link_address_v4(address, network_order_32(v4->host));
                address_to port = network_order_16(v4->port);
        }
        return got;
}

// The state file, for `moonwater link` -----------------------------------------

typedef struct
{
        p8 key[32];
        p8 address[16];
        p16 port;
        p64 seen;
} link_seen_entry;

static link_seen_entry link_seen[LINK_PEERS_MAX];
static positive link_seen_count;

static fn link_note_seen(struct link_session address_to s, p64 wall)
{
        positive at;

        for (at = 0; at < link_seen_count; at++)
                if (crypto_same(link_seen[at].key, s->peer, 32))
                        break;
        if (at == link_seen_count)
        {
                if (link_seen_count == LINK_PEERS_MAX)
                        return;
                link_seen_count++;
        }
        memory_copy(link_seen[at].key, s->peer, 32);
        memory_copy(link_seen[at].address, s->address, 16);
        link_seen[at].port = s->port;
        link_seen[at].seen = wall;
}

/*
        What the listener knows that the files do not: which peers it has
        heard, from where, and what is open. Written whole and renamed into
        place, never more than once a second.
*/
static fn link_state_write(p64 now)
{
        p8 text[8192];
        positive used = 0;
        p64 wall = system_clock_ns(0) / 1000000000ull;

        if (!link_self.state_dirty || now - link_self.state_written < 200000)
                return;
        link_self.state_dirty = false;
        link_self.state_written = now;

        (void)wall;
        text[0] = 0;
        for (positive at = 0; at < link_seen_count; at++)
        {
                p8 place[64];
                p8 key[48];

                link_key_text(link_seen[at].key, key);
                link_place_text(link_seen[at].address, link_seen[at].port,
                                place);
                link_append(text, address_of used, sizeof text, "seen ");
                link_append(text, address_of used, sizeof text,
                            (string_address)key);
                link_append(text, address_of used, sizeof text, " ");
                link_append_number(text, address_of used, sizeof text,
                                   link_seen[at].seen);
                link_append(text, address_of used, sizeof text, " ");
                link_append(text, address_of used, sizeof text,
                            (string_address)place);
                link_append(text, address_of used, sizeof text, "\n");
        }

        for (positive at = 0; at < LINK_SESSIONS; at++)
        {
                struct link_session address_to s = link_self.session + at;
                p8 place[64];

                if (!s->used || !s->now.live)
                        continue;
                link_place_text(s->address, s->port, place);
                link_append(text, address_of used, sizeof text, "session ");
                link_append(text, address_of used, sizeof text,
                            (string_address)s->name);
                link_append(text, address_of used, sizeof text,
                            s->kind == LINK_KIND_SHELL  ? " shell "
                            : s->kind == LINK_KIND_RUN  ? " run "
                            : s->kind == LINK_KIND_PUSH ? " push "
                            : s->kind == LINK_KIND_PULL ? " pull "
                            : s->kind == LINK_KIND_LOG  ? " log "
                                                        : " open ");
                link_append_number(text, address_of used, sizeof text,
                                   (now - s->opened) / 1000000);
                link_append(text, address_of used, sizeof text, " ");
                link_append_number(text, address_of used, sizeof text,
                                   s->link->smoothed);
                link_append(text, address_of used, sizeof text, " ");
                link_append(text, address_of used, sizeof text,
                            (string_address)place);
                link_append(text, address_of used, sizeof text, "\n");
        }

        if (host_write_file(LINK_STATE_NEXT, text, used, 0600, false) >= 0)
                system_rename_at(AT_FDCWD, LINK_STATE_NEXT, AT_FDCWD,
                                 LINK_STATE_PATH, 0);
}

#include "nearby.c"

// The listener ------------------------------------------------------------------

static bipolar link_socket_open(p16 port, bool any)
{
        bipolar handle = socket_new(AF_INET6,
                                    SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0);
        b32 zero = 0;
        b32 big = 4 << 20;
        socket_address_internet6 self;

        link_self.v4 = false;
        if (handle < 0)
        {
                socket_address_internet plain;

                handle = socket_new(AF_INET,
                                    SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0);
                if (handle < 0)
                        return handle;
                link_self.v4 = true;
                (void)socket_option_set((b32)handle, SOL_SOCKET, SO_RCVBUF,
                                        address_of big, sizeof big);
                (void)socket_option_set((b32)handle, SOL_SOCKET, SO_SNDBUF,
                                        address_of big, sizeof big);
                if (!any)
                        return handle;
                memory_zero(address_of plain, sizeof plain);
                plain.family = AF_INET;
                plain.port = network_order_16(port);
                if (socket_bind((b32)handle, address_of plain, sizeof plain) < 0)
                {
                        socket_close((b32)handle);
                        return -EADDRINUSE;
                }
                return handle;
        }

        (void)socket_option_set((b32)handle, 41, 26, address_of zero,
                                sizeof zero); // IPV6_V6ONLY off
        (void)socket_option_set((b32)handle, SOL_SOCKET, SO_RCVBUF,
                                address_of big, sizeof big);
        (void)socket_option_set((b32)handle, SOL_SOCKET, SO_SNDBUF,
                                address_of big, sizeof big);

        if (any)
        {
                memory_zero(address_of self, sizeof self);
                self.family = AF_INET6;
                self.port = network_order_16(port);
                if (socket_bind((b32)handle, address_of self, sizeof self) < 0)
                {
                        bipolar failed = -EADDRINUSE;

                        socket_close((b32)handle);
                        return failed;
                }
        }
        return handle;
}

static bipolar link_signals_open(void)
{
        positive blocked = 1ul << (1 - 1) | 1ul << (2 - 1) | 1ul << (15 - 1) |
                           1ul << (17 - 1) | 1ul << (13 - 1) | 1ul << (28 - 1);

        (void)system_signal_mask(0, address_of blocked, null, 8);
        return system_call_4(syscall(signalfd4), (positive)(bipolar)-1,
                             (positive)address_of blocked, 8,
                             O_CLOEXEC | O_NONBLOCK);
}

static fn link_signals_take(bipolar handle, b32 address_to last)
{
        p8 information[128];

        while (system_read_once(handle, information, sizeof information) ==
               sizeof information)
        {
                p32 signal;

                memory_copy(address_of signal, information, 4);
                if (signal != 17 && signal != 13)
                        address_to last = (b32)signal;
        }
}

/*
        `moonwater link serve`: the listener in the foreground. `link on` and
        the machine process start exactly this, detached.
*/
static b32 link_serve(void)
{
        system_poll_descriptor watch[3 + LINK_SESSIONS * 5];
        bipolar lock;
        bipolar signals;
        b32 stop = 0;
        p16 port = link_port();
        p8 datagram[WATERLINK_DATAGRAM + 16];

        if (link_identity(address_of link_self.me, true) < 0)
                return host_refuse("%s cannot be read or made\n", LINK_KEY_PATH);

        lock = link_lock_take();
        if (lock < 0)
        {
                string_format(log_error, host_label "the link is already on\n");
                log_flush();
                return LINK_EXIT_BUSY;
        }

        link_self.socket = link_socket_open(port, true);
        if (link_self.socket < 0)
                return host_fail("the link's port", link_self.socket);
        link_nearby.socket = -1;
        link_self.server = true;
        //      Segment runs unless told otherwise, which is how the cost of
        //      sending one datagram at a time is measured.
        link_self.gso = !file_environment((string_address) "WATERLINK_NO_SEGMENTS");
        link_self.state_dirty = true;

        signals = link_signals_open();

        for (;;)
        {
                p64 now = link_now();
                p64 wake = now + 1000000;
                positive count = 0;
                timespec limit;

                if (signals >= 0)
                        link_signals_take(signals, address_of stop);
                if (stop)
                        break;

                //      Each session: its streams, its sends, its timers.
                for (positive at = 0; at < LINK_SESSIONS; at++)
                {
                        struct link_session address_to s = link_self.session + at;
                        p64 due;

                        if (!s->used)
                                continue;

                        if (s->kind != LINK_KIND_NONE)
                                link_session_streams(s, now);

                        if (s->exit_sent && waterlink_idle(s->link))
                                s->finished = true;
                        if (now - s->heard > LINK_DEAD ||
                            (s->now.live &&
                             waterlink_session_spent(now - s->now.made,
                                                     s->now.counter)))
                                s->finished = true;

                        if (s->finished)
                        {
                                if (s->now.live && s->exit_sent)
                                        link_session_end(s, true);
                                link_session_close(s);
                                continue;
                        }

                        if (s->now.live)
                        {
                                link_session_flush(s, now);
                                if (now - s->spoke > LINK_KEEPALIVE)
                                        link_session_say(s,
                                                         WATERLINK_KIND_CARRY);
                        }

                        due = waterlink_wake(s->link, now);
                        if (due < wake)
                                wake = due;
                        if (s->exited && s->exited_at + 300000 < wake &&
                            (!s->output_read || !s->error_read))
                                wake = s->exited_at + 300000;
                }

                link_state_write(now);
                {
                        p64 due = link_nearby_tick(now);

                        if (due < wake)
                                wake = due;
                }

                watch[count].descriptor = (b32)link_self.socket;
                watch[count].events = SYSTEM_POLL_READ;
                count++;
                if (signals >= 0)
                {
                        watch[count].descriptor = (b32)signals;
                        watch[count].events = SYSTEM_POLL_READ;
                        count++;
                }
                if (link_nearby.socket >= 0)
                {
                        watch[count].descriptor = (b32)link_nearby.socket;
                        watch[count].events = SYSTEM_POLL_READ;
                        count++;
                }

                for (positive at = 0; at < LINK_SESSIONS; at++)
                {
                        struct link_session address_to s = link_self.session + at;
                        bool room;

                        if (!s->used || s->kind == LINK_KIND_NONE)
                                continue;
                        room = link_room(s);

                        if (s->terminal >= 0 && (room || s->pending_length))
                        {
                                watch[count].descriptor = (b32)s->terminal;
                                watch[count].events =
                                        (room && !s->output_read
                                                 ? SYSTEM_POLL_READ
                                                 : 0) |
                                        (s->pending_length ? SYSTEM_POLL_WRITE
                                                           : 0);
                                count++;
                        }
                        if (s->output >= 0 && room && !s->output_read)
                        {
                                watch[count].descriptor = (b32)s->output;
                                watch[count].events = SYSTEM_POLL_READ;
                                count++;
                        }
                        if (s->error >= 0 && room && !s->error_read)
                        {
                                watch[count].descriptor = (b32)s->error;
                                watch[count].events = SYSTEM_POLL_READ;
                                count++;
                        }
                        if (s->input >= 0 && s->pending_length)
                        {
                                watch[count].descriptor = (b32)s->input;
                                watch[count].events = SYSTEM_POLL_WRITE;
                                count++;
                        }
                        if (s->pidfd >= 0 && !s->exited)
                        {
                                watch[count].descriptor = (b32)s->pidfd;
                                watch[count].events = SYSTEM_POLL_READ;
                                count++;
                        }
                }

                now = link_now();
                if (wake < now)
                        wake = now;
                limit.tv_sec = (wake - now) / 1000000;
                limit.tv_nsec = (wake - now) % 1000000 * 1000;
                (void)system_poll_wait(watch, count, address_of limit, null);

                now = link_now();
                for (positive turn = 0; turn < 256; turn++)
                {
                        p8 address[16];
                        p16 from_port;
                        bipolar got = link_receive(datagram, address,
                                                   address_of from_port);
                        struct waterlink_datagram head;

                        if (got < 0)
                                break;
                        if (got < 16)
                                continue;
                        memory_copy(address_of head, datagram, 16);
                        if (head.kind == WATERLINK_KIND_INITIATE)
                                link_server_initiation(datagram, (positive)got,
                                                       address, from_port, now);
                        else if (head.kind == WATERLINK_KIND_CARRY ||
                                 head.kind == WATERLINK_KIND_CLOSE)
                                (void)link_carried(datagram, (positive)got,
                                                   address, from_port, now,
                                                   link_server_hear);
                        else if (head.kind >= WATERLINK_KIND_PAIR_1 &&
                                 head.kind <= WATERLINK_KIND_PAIR_3)
                                link_pair_datagram(datagram, (positive)got,
                                                   address, from_port, now);
                }
                link_nearby_receive(now);
        }

        link_nearby_stop();

        //      Off: every session is told, and every command hung up on.
        for (positive at = 0; at < LINK_SESSIONS; at++)
                if (link_self.session[at].used)
                {
                        link_session_end(link_self.session + at, true);
                        link_session_close(link_self.session + at);
                }
        system_remove_at(AT_FDCWD, LINK_STATE_PATH, 0);
        system_close(lock);
        return 0;
}

// The client ----------------------------------------------------------------------

typedef struct
{
        p8 kind;
        bool answered;
        bool refused;
        bool output_done;
        bool error_done;
        b32 status;
        p64 credit;
        p64 sent;
        bool input_done;
        bool raw;
        terminal_modes saved;
        p8 refusal[128];
        bipolar input;  // standard input, or the file a push sends
        bipolar output; // standard output, or the file a pull fills
        bool output_failed;
        p8 output_part[4096];
} link_client_state;

static link_client_state link_client = {.input = 0, .output = 1};

static fn link_client_hear(address_any context,
                           struct waterlink_frame address_to head,
                           p8 address_to payload)
{
        positive length = head->length;

        (void)context;
        if (!length)
                return;

        switch (head->key)
        {
        case LINK_KEY_ANSWER:
                link_client.answered = true;
                if (payload[0] == 'N')
                {
                        positive keep = length - 1 < sizeof link_client.refusal - 1
                                                ? length - 1
                                                : sizeof link_client.refusal - 1;

                        //      Printed to a terminal: printable bytes only.
                        for (positive at = 0; at < keep; at++)
                                link_client.refusal[at] =
                                        payload[1 + at] >= ' ' &&
                                                        payload[1 + at] < 127
                                                ? payload[1 + at]
                                                : '?';
                        link_client.refusal[keep] = 0;
                        link_client.refused = true;
                }
                break;
        case LINK_KEY_CREDIT:
                if (length >= 9)
                        memory_copy(address_of link_client.credit, payload + 1,
                                    8);
                break;
        case LINK_KEY_OUTPUT:
                if (payload[0] == LINK_DATA &&
                    system_write_all((positive)link_client.output, payload + 1,
                                     length - 1) != length - 1)
                        link_client.output_failed = true;
                else if (payload[0] == LINK_EXIT && length >= 5)
                {
                        memory_copy(address_of link_client.status, payload + 1,
                                    4);
                        link_client.output_done = true;
                }
                break;
        case LINK_KEY_ERROR:
                if (payload[0] == LINK_DATA)
                        (void)system_write_all(2, payload + 1, length - 1);
                else
                        link_client.error_done = true;
                break;
        default:
                break;
        }
}

static fn link_client_restore(void)
{
        if (link_client.raw)
        {
                system_control(0, PTY_TCSETS, address_of link_client.saved);
                link_client.raw = false;
        }
}

static bool link_client_raw(void)
{
        terminal_modes raw;

        if (system_control(0, PTY_TCGETS, address_of link_client.saved) < 0)
                return false;
        raw = link_client.saved;
        raw.arriving &= ~(0001u | 0002u | 0010u | 0040u | 0100u | 0200u |
                          0400u | 02000u);
        raw.leaving &= ~0001u;
        raw.behaviour &= ~(0010u | 0100u | 0002u | 0001u | 0100000u);
        raw.hardware &= ~(0060u | 0400u);
        raw.hardware |= 0060u;
        raw.controls[6] = 1;
        raw.controls[5] = 0;
        if (system_control(0, PTY_TCSETS, address_of raw) < 0)
                return false;
        link_client.raw = true;
        return true;
}

/*
        The initiator's half of the handshake, for a new session or to key the
        one it has again. Returns the answer's datagram once it verifies.
*/
static bool link_client_handshake(struct link_session address_to s,
                                  struct waterlink_noise address_to noise,
                                  p32 ours, p8 address_to datagram)
{
        p8 hello[WATERLINK_HELLO_BYTES];
        p8 ephemeral[32];
        p64 wall = system_clock_ns(0);

        waterlink_stamp(hello, wall / 1000000000ull,
                        (p32)(wall % 1000000000ull));
        memory_copy(hello + WATERLINK_STAMP_BYTES, address_of s->conversation, 8);
        memory_copy(hello + WATERLINK_STAMP_BYTES + 8, address_of ours, 4);
        if (system_random_fill(ephemeral, 32, 0) < 0)
        {
                crypto_forget(ephemeral, sizeof ephemeral);
                crypto_forget(noise, sizeof(address_to noise));
                return false;
        }
        if (!waterlink_initiate(noise, address_of link_self.me, s->peer,
                                ephemeral, hello, datagram))
        {
                crypto_forget(ephemeral, sizeof ephemeral);
                crypto_forget(noise, sizeof(address_to noise));
                return false;
        }
        crypto_forget(ephemeral, sizeof ephemeral);
        return link_send_to(datagram, WATERLINK_DATAGRAM, s->address,
                            s->port) >= 0;
}

static bool link_client_answer(struct link_session address_to s,
                               struct waterlink_noise address_to noise,
                               p32 ours, p8 address_to datagram,
                               positive length)
{
        struct waterlink_datagram head;
        struct waterlink_noise candidate;
        p8 send[16], receive[16];
        p32 theirs = 0;

        memory_copy(address_of head, datagram, 16);
        if (head.kind != WATERLINK_KIND_RESPOND || head.receiver != ours ||
            !waterlink_gate_passes(address_of link_self.me, datagram, length))
                return false;

        /* A forged answer must not advance the live transcript and spoil the
           real answer which follows it. Commit the candidate only after every
           DH and tag has verified. */
        candidate = *noise;
        if (!waterlink_answered(address_of candidate, address_of link_self.me,
                                datagram, address_of theirs))
        {
                crypto_forget(address_of candidate, sizeof candidate);
                return false;
        }

        waterlink_split(address_of candidate, true, send, receive);
        crypto_forget(noise, sizeof(address_to noise));
        if (s->now.live)
                s->before = s->now;
        link_keys_install(address_of s->now, send, receive, ours, theirs);
        crypto_forget(send, sizeof send);
        crypto_forget(receive, sizeof receive);
        return true;
}

static p64 link_rekey_after(void)
{
        string_address text = file_environment(
            (string_address) "WATERLINK_REKEY_SECONDS");
        bipolar seconds = link_decimal(text);

        //      Sooner is always safe, and is how a test sees a rekey.
        if (seconds > 0 && seconds < WATERLINK_REKEY_SECONDS)
                return (p64)seconds * 1000000;
        return (p64)WATERLINK_REKEY_SECONDS * 1000000;
}

static b32 link_client_run(string_address name, p8 kind,
                           string_address address_to words, positive count)
{
        link_peers peers;
        struct waterlink_peer address_to peer;
        struct link_session address_to s = link_self.session;
        struct waterlink_noise noise;
        struct waterlink_noise renoise;
        p8 datagram[WATERLINK_DATAGRAM + 16];
        p8 request[LINK_REQUEST_MAX + 64];
        positive request_length = 0;
        p32 ours;
        p32 reours = 0;
        p64 started;
        p64 rekey_after = link_rekey_after();
        p64 rekey_sent = 0;
        bool keyed = false;
        bipolar signals;
        b32 stopped = 0;
        b32 answer = LINK_FAILED;

        if (link_identity(address_of link_self.me, false) < 0)
                return host_refuse("this machine has no link key; "
                                   "moonwater link key makes one%s\n",
                                   "");

        link_peers_load(address_of peers);
        peer = link_peer_named(address_of peers, name);
        if (!peer)
                return host_refuse("no peer is called %s\n", name);
        if (!peer->port)
                return host_refuse("%s has no address; pair it again with "
                                   "one\n",
                                   name);

        //      The request, before anything is sent: a command that cannot be
        //      asked for is refused here.
        request[request_length++] = kind == LINK_KIND_SHELL ? LINK_ASK_SHELL
                                    : kind == LINK_KIND_PUSH ? LINK_ASK_PUSH
                                    : kind == LINK_KIND_PULL ? LINK_ASK_PULL
                                    : kind == LINK_KIND_LOG  ? LINK_ASK_LOG
                                                             : LINK_ASK_RUN;
        if (kind == LINK_KIND_PUSH || kind == LINK_KIND_PULL)
        {
                //      words: the source, then where it goes.
                string_address far = kind == LINK_KIND_PUSH ? words[1] : words[0];
                positive length = string_length(far);

                if (!length || length > LINK_REQUEST_MAX - 8)
                        return host_refuse("%s is not a path the far side "
                                           "can take\n",
                                           far);
                if (kind == LINK_KIND_PUSH)
                {
                        file_facts facts;
                        p32 mode;

                        link_client.input = system_open_at(AT_FDCWD, words[0],
                                                           FILE_READ | O_CLOEXEC);
                        if (link_client.input < 0)
                                return host_fail(words[0], link_client.input);
                        mode = file_look(link_client.input, (string_address) "",
                                         AT_EMPTY_PATH, address_of facts)
                                       ? facts.mode & 0777
                                       : 0644;
                        memory_copy(request + request_length, address_of mode, 4);
                        request_length += 4;
                }
                memory_copy(request + request_length, far, length);
                request_length += length;
        }
        else if (kind == LINK_KIND_LOG)
                ;
        else if (kind == LINK_KIND_SHELL)
        {
                winsize size = {24, 80, 0, 0};
                string_address term = file_environment((string_address) "TERM");

                system_control(0, TIOCGWINSZ, address_of size);
                request[request_length++] = (p8)size.rows;
                request[request_length++] = (p8)(size.rows >> 8);
                request[request_length++] = (p8)size.columns;
                request[request_length++] = (p8)(size.columns >> 8);
                if (term && string_length(term) < 32)
                {
                        memory_copy(request + request_length, term,
                                    string_length(term));
                        request_length += string_length(term);
                }
        }
        else
        {
                for (positive at = 0; at < count; at++)
                {
                        positive length = string_length(words[at]);

                        if (request_length + length + 1 > LINK_REQUEST_MAX)
                                return host_refuse("the command is longer than "
                                                   "%s bytes\n",
                                                   "1024");
                        if (at)
                                request[request_length++] = ' ';
                        memory_copy(request + request_length, words[at],
                                    length);
                        request_length += length;
                }
        }

        link_self.socket = link_socket_open(0, false);
        if (link_self.socket < 0)
                return host_fail("a socket", link_self.socket);
        link_self.gso = !file_environment((string_address) "WATERLINK_NO_SEGMENTS");

        if (!link_session_open(s))
                return host_fail("memory", -ENOMEM);
        memory_copy(s->peer, peer->key, 32);
        memory_copy(s->name, peer->name, WATERLINK_NAME_MAX);
        memory_copy(s->address, peer->address, 16);
        s->port = peer->port;
        if (system_random_fill(address_of s->conversation, 8, 0) < 0)
        {
                link_session_close(s);
                return host_fail("randomness", -EIO);
        }

        //      The handshake: a new initiation a second until one is answered.
        ours = link_index_new();
        if (!ours)
        {
                link_session_close(s);
                return host_fail("randomness", -EIO);
        }
        for (positive attempt = 0; attempt < LINK_ATTEMPTS && !keyed; attempt++)
        {
                p64 until;

                link_client_handshake(s, address_of noise, ours, datagram);
                until = link_now() + LINK_ATTEMPT;
                while (!keyed && link_now() < until)
                {
                        timespec limit = {0, 50000000};
                        system_poll_descriptor wait = {(b32)link_self.socket,
                                                       SYSTEM_POLL_READ, 0};
                        p8 address[16];
                        p16 port;
                        bipolar got;

                        (void)system_poll_wait(address_of wait, 1,
                                               address_of limit, null);
                        while ((got = link_receive(datagram, address,
                                                   address_of port)) > 0)
                                if (link_client_answer(s, address_of noise,
                                                       ours, datagram,
                                                       (positive)got))
                                {
                                        keyed = true;
                                        break;
                                }
                }
        }

        if (!keyed)
        {
                p8 place[64];

                link_place_text(s->address, s->port, place);
                string_format(log_error,
                              host_label "%s did not answer at %s: it is off, "
                                         "unreachable, or does not know this "
                                         "machine's key\n",
                              name, (string_address)place);
                log_flush();
                link_session_close(s);
                return LINK_FAILED;
        }

        if (kind == LINK_KIND_PULL)
        {
                positive length = string_length(words[1]);

                if (length + 28 > sizeof link_client.output_part)
                        return host_refuse("%s is too long a name\n", words[1]);
                link_client.output = link_part_open(
                        words[1], length, link_client.output_part,
                        sizeof link_client.output_part, 0644);
                if (link_client.output < 0)
                        return host_fail((string_address)link_client.output_part,
                                         link_client.output);
        }

        link_client.kind = kind;
        link_client.credit = kind == LINK_KIND_SHELL ? ~0ull : 0;
        if (kind == LINK_KIND_PULL || kind == LINK_KIND_LOG)
                link_client.input_done = true;
        (void)waterlink_post(s->link, LINK_KEY_REQUEST, 0,
                             WATERLINK_FRAME_DURABLE | WATERLINK_FRAME_URGENT |
                                     WATERLINK_FRAME_LAST,
                             0, 0, request, (p16)request_length, link_now());

        signals = link_signals_open();
        if (kind == LINK_KIND_SHELL)
                (void)link_client_raw();
        started = link_now();

        for (;;)
        {
                system_poll_descriptor watch[3];
                positive watching = 0;
                p64 now = link_now();
                p64 wake;
                timespec limit;
                bool read_input;

                if (signals >= 0)
                        link_signals_take(signals, address_of stopped);

                //      A window's new size replaces the one not yet sent.
                if (stopped == 28)
                {
                        winsize size;
                        p8 packed[4];

                        stopped = 0;
                        if (system_control(0, TIOCGWINSZ, address_of size) >= 0)
                        {
                                packed[0] = (p8)size.rows;
                                packed[1] = (p8)(size.rows >> 8);
                                packed[2] = (p8)size.columns;
                                packed[3] = (p8)(size.columns >> 8);
                                (void)link_post(s, LINK_KEY_SIZE,
                                                WATERLINK_FRAME_REPLACEABLE |
                                                        WATERLINK_FRAME_URGENT,
                                                'W', packed, 4);
                        }
                }
                else if (stopped == 2 && kind == LINK_KIND_RUN)
                {
                        //      ^C with no terminal: the command gets it.
                        p8 signal = 2;

                        stopped = 0;
                        (void)link_post(s, LINK_KEY_SIGNAL,
                                        WATERLINK_FRAME_DURABLE |
                                                WATERLINK_FRAME_URGENT,
                                        'K', address_of signal, 1);
                }
                else if (stopped)
                {
                        link_session_end(s, true);
                        answer = 128 + stopped;
                        break;
                }

                if (link_client.refused)
                {
                        link_client_restore();
                        string_format(log_error, host_label "%s: %s\n", name,
                                      (string_address)link_client.refusal);
                        log_flush();
                        link_session_end(s, true);
                        answer = LINK_FAILED;
                        break;
                }

                if (link_client.output_done &&
                    (kind != LINK_KIND_RUN || link_client.error_done))
                {
                        //      Acknowledge what ended it, then say goodbye.
                        link_session_flush(s, now);
                        link_session_end(s, true);
                        link_session_say(s, WATERLINK_KIND_CLOSE);
                        answer = link_client.status;
                        break;
                }

                if (now - s->heard > LINK_DEAD || s->finished)
                {
                        link_client_restore();
                        string_format(log_error,
                                      s->finished
                                              ? host_label "%s closed the link\n"
                                              : host_label "the link to %s "
                                                           "was lost\n",
                                      name);
                        log_flush();
                        answer = LINK_FAILED;
                        break;
                }

                //      Keyed again before the session runs out, under the same
                //      conversation, so the streams go on as they were.
                if (!rekey_sent && now - s->now.made >= rekey_after)
                {
                        reours = link_index_new();
                        if (!reours ||
                            !link_client_handshake(s, address_of renoise,
                                                   reours, datagram))
                        {
                                crypto_forget(address_of renoise,
                                              sizeof renoise);
                                answer = LINK_FAILED;
                                break;
                        }
                        else
                                rekey_sent = now;
                }
                else if (rekey_sent && now - rekey_sent > LINK_ATTEMPT)
                        rekey_sent = 0;

                link_session_flush(s, now);
                if (now - s->spoke > LINK_KEEPALIVE)
                        link_session_say(s, WATERLINK_KIND_CARRY);

                wake = waterlink_wake(s->link, now);
                if (wake > now + 1000000)
                        wake = now + 1000000;

                watch[watching].descriptor = (b32)link_self.socket;
                watch[watching].events = SYSTEM_POLL_READ;
                watching++;
                if (signals >= 0)
                {
                        watch[watching].descriptor = (b32)signals;
                        watch[watching].events = SYSTEM_POLL_READ;
                        watching++;
                }
                read_input = !link_client.input_done && link_room(s) &&
                             link_client.answered &&
                             link_client.sent < link_client.credit;
                if (read_input)
                {
                        watch[watching].descriptor = (b32)link_client.input;
                        watch[watching].events = SYSTEM_POLL_READ;
                        watching++;
                }

                now = link_now();
                if (wake < now)
                        wake = now;
                limit.tv_sec = (wake - now) / 1000000;
                limit.tv_nsec = (wake - now) % 1000000 * 1000;
                (void)system_poll_wait(watch, watching, address_of limit, null);
                now = link_now();

                if (read_input && (watch[watching - 1].returned &
                                   (SYSTEM_POLL_READ | SYSTEM_POLL_HANGUP |
                                    SYSTEM_POLL_ERROR)))
                {
                        positive room = link_room_frames(s) * LINK_CHUNK;
                        bipolar got;

                        if (link_client.credit - link_client.sent < room)
                                room = (positive)(link_client.credit -
                                                  link_client.sent);
                        got = system_read_once(link_client.input,
                                               link_read_buffer, room);
                        if (got > 0)
                        {
                                //      A keystroke leaves alone and at once.
                                for (positive at = 0; at < (positive)got;
                                     at += LINK_CHUNK)
                                        (void)link_post(
                                                s, LINK_KEY_INPUT,
                                                WATERLINK_FRAME_DURABLE |
                                                        (kind == LINK_KIND_SHELL
                                                                 ? WATERLINK_FRAME_URGENT
                                                                 : 0),
                                                LINK_DATA, link_read_buffer + at,
                                                (positive)got - at < LINK_CHUNK
                                                        ? (positive)got - at
                                                        : LINK_CHUNK);
                                link_client.sent += (positive)got;
                                link_session_flush(s, now);
                        }
                        else if (got != -EAGAIN && got != -4)
                        {
                                link_client.input_done = true;
                                (void)link_post(s, LINK_KEY_INPUT,
                                                WATERLINK_FRAME_DURABLE |
                                                        WATERLINK_FRAME_LAST,
                                                LINK_END, null, 0);
                        }
                }

                for (positive turn = 0; turn < 256; turn++)
                {
                        p8 address[16];
                        p16 port;
                        bipolar got = link_receive(datagram, address,
                                                   address_of port);
                        struct waterlink_datagram head;

                        if (got < 16)
                                break;
                        memory_copy(address_of head, datagram, 16);
                        if (head.kind == WATERLINK_KIND_RESPOND && rekey_sent)
                        {
                                if (link_client_answer(s, address_of renoise,
                                                       reours, datagram,
                                                       (positive)got))
                                        rekey_sent = 0;
                        }
                        else if (head.kind == WATERLINK_KIND_CARRY ||
                                 head.kind == WATERLINK_KIND_CLOSE)
                                (void)link_carried(datagram, (positive)got,
                                                   address, port, now,
                                                   link_client_hear);
                }
        }

        (void)started;
        link_client_restore();

        //      A pulled file is only there under its name once it is whole.
        if (kind == LINK_KIND_PULL && link_client.output >= 0)
        {
                file_facts opened;
                file_facts named;
                bool owned = file_look(link_client.output, (string_address)"",
                                       AT_EMPTY_PATH, address_of opened) &&
                             file_look(AT_FDCWD,
                                       (string_address)link_client.output_part,
                                       AT_SYMLINK_NOFOLLOW, address_of named) &&
                             file_same_identity(address_of opened,
                                                address_of named);

                if (!answer && !link_client.output_failed &&
                    owned &&
                    system_call_1(syscall(fsync), (positive)link_client.output) >= 0 &&
                    system_rename_at(AT_FDCWD, link_client.output_part,
                                     AT_FDCWD, words[1], 0) >= 0)
                        ;
                else
                {
                        if (owned)
                                system_remove_at(AT_FDCWD,
                                                 link_client.output_part, 0);
                        if (!answer)
                                answer = 1;
                }
                system_close(link_client.output);
        }
        //      What the link did, for whoever is measuring it.
        if (file_environment((string_address) "WATERLINK_STATS"))
        {
                struct waterlink_link address_to l = s->link;

                string_format(log_error,
                              "link: sent %p again %p lost %p timeouts %p "
                              "delivered %p held %p spilled %p rtt %p us "
                              "window %p\n",
                              (positive)l->sent, (positive)l->retransmitted,
                              (positive)l->lost, (positive)l->timeouts,
                              (positive)l->delivered, (positive)l->kept,
                              (positive)l->spilled, (positive)l->smoothed,
                              (positive)l->window);
                log_flush();
        }
        if (rekey_after < (p64)WATERLINK_REKEY_SECONDS * 1000000)
        {
                string_format(log_error, host_label "keyed %p times\n",
                              (positive)s->keyings);
                log_flush();
        }
        link_session_close(s);
        return answer;
}

#endif // WATERLINK_SERVICE_INCLUDED
