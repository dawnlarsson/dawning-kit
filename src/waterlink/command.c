/*
        `moonwater link`: the verbs.

                link                    what is on, this machine's key, the
                                        peers, what is open
                link on | off           the listener, kept across boots
                link key                this machine's public key
                link pair NAME KEY [HOST[:PORT]]
                link forget NAME
                link allow NAME GRANT...
                link deny NAME GRANT...
                link shell NAME         a terminal on NAME
                link run NAME COMMAND...
                link push NAME FILE PATH   a file there, whole or not at all
                link pull NAME PATH FILE   and back
                link log NAME           follow the far kernel log
                link serve              the listener, in the foreground

        Pairing is by key and both ways, as WireGuard's is: each machine is
        told the other's key, and a machine answers only a key it was told.
        A new peer may do nothing but the moonwater verbs; everything past
        that is a grant somebody gave it by name.

        The switch, the key and the peers live in /root, beside the wireless
        networks, so install carries them to the disk and wipe keeps them.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit
*/

#ifndef WATERLINK_COMMAND_INCLUDED
#define WATERLINK_COMMAND_INCLUDED

#include "service.c"

static fn link_usage_write(writer out)
{
        string_format(out,
                      TERM_BOLD "  link" TERM_RESET
                      "                        " TERM_DIM "on or off, this machine's key, peers, sessions" TERM_RESET "\n"
                      TERM_BOLD "  link on|off" TERM_RESET
                      "                 " TERM_DIM "the listener, udp 22348, kept across boots" TERM_RESET "\n"
                      TERM_BOLD "  link key" TERM_RESET
                      "                    " TERM_DIM "this machine's public key, made on first use" TERM_RESET "\n"
                      TERM_BOLD "  link pair NAME KEY [HOST[:PORT]]" TERM_RESET "\n"
                      "                              " TERM_DIM "know a machine by its key" TERM_RESET "\n"
                      TERM_BOLD "  link forget NAME" TERM_RESET
                      "            " TERM_DIM "stop knowing it" TERM_RESET "\n"
                      TERM_BOLD "  link allow|deny NAME GRANT..." TERM_RESET "\n"
                      "                              " TERM_DIM "run shell files log screen channels verbs" TERM_RESET "\n"
                      TERM_BOLD "  link shell NAME" TERM_RESET
                      "             " TERM_DIM "a terminal on NAME" TERM_RESET "\n"
                      TERM_BOLD "  link run NAME COMMAND..." TERM_RESET
                      "    " TERM_DIM "one command on NAME, its output and status here" TERM_RESET "\n"
                      TERM_BOLD "  link push NAME FILE PATH" TERM_RESET
                      "    " TERM_DIM "a file here to PATH there" TERM_RESET "\n"
                      TERM_BOLD "  link pull NAME PATH FILE" TERM_RESET
                      "    " TERM_DIM "PATH there to a file here" TERM_RESET "\n"
                      TERM_BOLD "  link log NAME" TERM_RESET
                      "               " TERM_DIM "follow NAME's kernel log" TERM_RESET "\n"
                      TERM_BOLD "  link serve" TERM_RESET
                      "                  " TERM_DIM "the listener in the foreground" TERM_RESET "\n");
        log_flush();
}

static b32 link_usage(void)
{
        link_usage_write(log_error);
        return 2;
}

static fn link_grants_text(p32 may, p8 address_to text, positive room)
{
        positive used = 0;

        text[0] = 0;
        for (positive at = 0; at < array_count(link_grants); at++)
                if (may & link_grants[at].bit)
                {
                        if (used)
                                link_append(text, address_of used, room, " ");
                        link_append(text, address_of used, room,
                                    link_grants[at].name);
                }
        if (!used)
                link_append(text, address_of used, room, "nothing");
}

static fn link_ago(p64 then, p8 address_to text)
{
        p64 wall = system_clock_ns(0) / 1000000000ull;
        p64 gone = wall > then ? wall - then : 0;
        positive used = 0;

        text[0] = 0;
        if (gone < 120)
        {
                link_append_number(text, address_of used, 32, gone);
                link_append(text, address_of used, 32, "s ago");
        }
        else if (gone < 7200)
        {
                link_append_number(text, address_of used, 32, gone / 60);
                link_append(text, address_of used, 32, "m ago");
        }
        else
        {
                link_append_number(text, address_of used, 32, gone / 3600);
                link_append(text, address_of used, 32, "h ago");
        }
}

/*
        The page: whether it is on and where, the key to give other machines,
        every peer with what it may do and when and where it was last heard,
        and what is open now. Everything here came out of this machine's own
        files; a place is digits the listener wrote.
*/
static b32 link_status(void)
{
        struct waterlink_identity me;
        link_peers peers;
        p8 state[8192];
        positive state_length = 0;
        bipolar owner = link_lock_owner();
        p8 key[48];

        link_read_exact(LINK_STATE_PATH, state, sizeof state - 1,
                        address_of state_length);
        state[state_length] = 0;

        {
                p8 word[16];

                host_read_text(LINK_SWITCH_PATH, word, sizeof word);
                string_format(
                        log, host_label "link %s%s, udp %p\n",
                        owner > 0 ? "on" : "off",
                        owner > 0 && string_equals((string_address)word, "on")
                                ? ""
                        : owner > 0 && !word[0] ? ", run by hand"
                        : owner > 0             ? ", switched off but still running"
                        : string_equals((string_address)word, "on")
                                ? ", switched on but not running"
                                : "",
                        (positive)link_port());
        }

        if (link_identity(address_of me, false) >= 0)
        {
                link_key_text(me.public, key);
                string_format(log, "  this machine  %s\n", (string_address)key);
                crypto_forget(address_of me, sizeof me);
        }
        else
                string_format(log, "  this machine  no key yet: moonwater link key\n");

        link_peers_load(address_of peers);
        if (!peers.count)
                string_format(log, "  no peers: moonwater link pair NAME KEY HOST\n");

        for (positive at = 0; at < peers.count; at++)
        {
                struct waterlink_peer address_to peer = peers.peer + at;
                p8 grants[96];
                p8 place[64];
                p8 short_key[12];
                string_address seen = null;

                link_key_text(peer->key, key);
                memory_copy(short_key, key, 8);
                short_key[8] = 0;
                link_grants_text(peer->may, grants, sizeof grants);

                string_format(log, "  %s  %s...  may %s\n",
                              (string_address)peer->name,
                              (string_address)short_key,
                              (string_address)grants);

                //      The listener's word on where it was last heard, over
                //      the address it was paired with.
                for (string_address line = (string_address)state; line && *line;)
                {
                        string_address next = string_first_of(line, '\n');

                        if (host_starts(line, "seen ") &&
                            !memory_compare(line + 5, key, 44))
                                seen = line + 50;
                        line = next ? next + 1 : null;
                }

                if (seen)
                {
                        p8 when[32];
                        bipolar seconds = 0;
                        string_address at_place;

                        while (*seen >= '0' && *seen <= '9')
                                seconds = seconds * 10 + (*seen++ - '0');
                        at_place = seen + 1;
                        link_ago((p64)seconds, when);
                        {
                                string_address end_line =
                                        string_first_of(at_place, '\n');
                                positive length = end_line
                                                          ? (positive)(end_line -
                                                                       at_place)
                                                          : string_length(at_place);

                                if (length >= sizeof place)
                                        length = sizeof place - 1;
                                memory_copy(place, at_place, length);
                                place[length] = 0;
                        }
                        string_format(log, "      heard %s from %s\n",
                                      (string_address)when,
                                      (string_address)place);
                }
                else if (peer->port)
                {
                        link_place_text(peer->address, peer->port, place);
                        string_format(log, "      at %s, not heard this boot\n",
                                      (string_address)place);
                }
                else
                        string_format(log, "      no address; it can reach "
                                           "this machine, not the other way\n");
        }

        {
                bool any = false;

                for (string_address line = (string_address)state; line && *line;)
                {
                        string_address next = string_first_of(line, '\n');

                        if (host_starts(line, "session "))
                        {
                                //      name kind seconds rtt place
                                string_address field[5] = {null};
                                positive fields = 0;
                                string_address at = line + 8;

                                if (next)
                                        *(p8 address_to)next = 0;
                                while (fields < 5 && at && *at)
                                {
                                        string_address space =
                                                string_first_of(at, ' ');

                                        field[fields++] = at;
                                        if (space && fields < 5)
                                                *(p8 address_to)space = 0;
                                        at = space && fields < 5 ? space + 1
                                                                 : null;
                                }
                                if (fields == 5)
                                {
                                        bipolar rtt = link_decimal(field[3]);

                                        if (rtt < 0)
                                                rtt = 0;

                                        if (!any)
                                                string_format(log, "  open\n");
                                        any = true;
                                        string_format(log,
                                                      "    %s  %s  %ss  rtt "
                                                      "%p.%p ms  from %s\n",
                                                      field[0], field[1],
                                                      field[2],
                                                      (positive)(rtt / 1000),
                                                      (positive)(rtt % 1000 / 100),
                                                      field[4]);
                                }
                        }
                        line = next ? next + 1 : null;
                }
        }

        string_format(log, "\n");
        link_usage_write(log);
        return 0;
}

static b32 link_key_verb(void)
{
        struct waterlink_identity me;
        p8 key[48];
        bipolar got = link_identity(address_of me, true);

        if (got == -EPERM)
                return host_refuse("%s is not a private key: it must be a "
                                   "32-byte file only root can read\n",
                                   LINK_KEY_PATH);
        if (got < 0)
                return host_fail(LINK_KEY_PATH, got);

        link_key_text(me.public, key);
        crypto_forget(address_of me, sizeof me);
        string_format(log, "%s\n", (string_address)key);
        log_flush();
        return 0;
}

static b32 link_pair(string_address name, string_address text,
                     string_address place)
{
        link_peers peers;
        struct waterlink_peer peer;
        struct waterlink_peer address_to found;
        struct waterlink_identity me;

        if (!link_name_good(name))
                return host_refuse("%s is not a peer name: letters, digits, "
                                   ". - _, up to 31\n",
                                   name);

        memory_zero(address_of peer, sizeof peer);
        if (!link_key_parse(text, peer.key))
                return host_refuse("%s is not a link key: 44 characters of "
                                   "base64, as moonwater link key prints\n",
                                   text);

        if (link_identity(address_of me, false) >= 0 &&
            crypto_same(me.public, peer.key, 32))
                return host_refuse("that is this machine's own key%s\n", "");

        if (place && !link_parse_place(place, peer.address, address_of peer.port))
                return host_refuse("%s is not an address this can reach\n",
                                   place);

        string_copy(peer.name, name);
        peer.may = WATERLINK_MAY_DEFAULT;

        link_peers_load(address_of peers);
        found = link_peer_named(address_of peers, name);
        if (!found)
                found = link_peer_keyed(address_of peers, peer.key);

        if (found)
        {
                //      Paired again: the grants stay with the key they were
                //      given to, and nowhere else.
                if (crypto_same(found->key, peer.key, 32))
                        peer.may = found->may;
                *found = peer;
        }
        else
        {
                if (peers.count >= LINK_PEERS_MAX)
                        return host_refuse("this machine knows %s peers "
                                           "already\n",
                                           "64");
                peers.peer[peers.count++] = peer;
        }

        if (link_peers_save(address_of peers) < 0)
                return host_refuse("%s could not be written\n", LINK_PEERS_PATH);

        string_format(log, host_label "paired %s; it may use the moonwater "
                                      "verbs, and more once allowed\n",
                      name);
        log_flush();
        return 0;
}

static b32 link_forget(string_address name)
{
        link_peers peers;
        struct waterlink_peer address_to found;

        link_peers_load(address_of peers);
        found = link_peer_named(address_of peers, name);
        if (!found)
                return host_refuse("no peer is called %s\n", name);

        *found = peers.peer[--peers.count];
        if (link_peers_save(address_of peers) < 0)
                return host_refuse("%s could not be written\n", LINK_PEERS_PATH);

        string_format(log, host_label "forgot %s\n", name);
        log_flush();
        return 0;
}

static b32 link_grant_verb(string_address name, bool allow,
                      string_address address_to words, positive count)
{
        link_peers peers;
        struct waterlink_peer address_to found;
        p8 grants[96];

        link_peers_load(address_of peers);
        found = link_peer_named(address_of peers, name);
        if (!found)
                return host_refuse("no peer is called %s\n", name);

        for (positive at = 0; at < count; at++)
        {
                p32 bit = 0;

                for (positive look = 0; look < array_count(link_grants); look++)
                        if (string_equals(words[at], link_grants[look].name))
                                bit = link_grants[look].bit;
                if (!bit)
                        return host_refuse("%s is not a grant: run shell files "
                                           "log screen channels verbs\n",
                                           words[at]);
                if (allow)
                        found->may |= bit;
                else
                        found->may &= ~bit;
        }

        if (link_peers_save(address_of peers) < 0)
                return host_refuse("%s could not be written\n", LINK_PEERS_PATH);

        link_grants_text(found->may, grants, sizeof grants);
        string_format(log, host_label "%s may %s\n", name,
                      (string_address)grants);
        log_flush();
        return 0;
}

/*
        The listener, detached from whoever asked: a new session with nothing
        open but /dev/null, and not this process's child, so a terminal closing
        takes nothing with it. The machine process restarts it if it dies.
*/
static bipolar link_start_detached(void)
{
        bipolar child = system_fork();

        if (child < 0)
                return child;
        if (!child)
        {
                string_address words[] = {"moonwater", "link", "serve", null};
                bipolar null_handle;

                (void)system_call(syscall(setsid));
                if (system_fork())
                        system_call_1(syscall(exit), 0);
                null_handle = system_open_at(AT_FDCWD, "/dev/null",
                                             FILE_READ_WRITE);
                if (null_handle >= 0)
                        for (b32 target = 0; target < 3; target++)
                                system_descriptor_install(null_handle, target);
                (void)system_call_3(syscall(close_range), 3, ~0u, 0);
                (void)shell_exec_file((string_address) "/proc/self/exe", words,
                                      3, file_environment_all());
                system_call_1(syscall(exit), 127);
        }

        (void)system_call_4(syscall(wait4), (positive)child, 0, 0, 0);
        return 0;
}

static bool link_wait_owner(bool present)
{
        for (positive turn = 0; turn < 60; turn++)
        {
                if ((link_lock_owner() > 0) == present)
                        return true;
                host_pause(50000000);
        }
        return false;
}

static b32 link_switch(bool on)
{
        struct waterlink_identity me;
        p8 key[48];

        if (radio_write_word(LINK_SWITCH_PATH, on ? "on" : "off") < 0)
                return host_refuse("%s could not be written\n",
                                   LINK_SWITCH_PATH);

        if (!on)
        {
                (void)link_lock_signal(15);
                if (!link_wait_owner(false))
                        return host_refuse("the listener did not stop%s\n", "");
                string_format(log, host_label "link off; sessions closed\n");
                log_flush();
                return 0;
        }

        if (link_identity(address_of me, true) < 0)
                return host_refuse("%s cannot be read or made\n", LINK_KEY_PATH);

        if (link_lock_owner() <= 0)
                (void)link_start_detached();
        if (!link_wait_owner(true))
                return host_refuse("the listener did not start: is udp %s "
                                   "taken?\n",
                                   "22348");

        link_key_text(me.public, key);
        crypto_forget(address_of me, sizeof me);
        string_format(log, host_label "link on, udp %p; this machine is %s\n",
                      (positive)link_port(), (string_address)key);
        log_flush();
        return 0;
}

static b32 link_main(string_address address_to arguments, positive count)
{
        string_address verb = count > 2 ? arguments[2] : null;

        if (!verb)
                return link_status();

        if (string_equals(verb, "-h") || string_equals(verb, "--help") ||
            string_equals(verb, "help"))
        {
                link_usage_write(log);
                return 0;
        }

        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater link");

        if ((string_equals(verb, "on") || string_equals(verb, "off")) &&
            count == 3)
                return link_switch(string_equals(verb, "on"));
        if (string_equals(verb, "key") && count == 3)
                return link_key_verb();
        if (string_equals(verb, "serve") && count == 3)
                return link_serve();
        if (string_equals(verb, "pair") && (count == 5 || count == 6))
                return link_pair(arguments[3], arguments[4],
                                 count == 6 ? arguments[5] : null);
        if (string_equals(verb, "forget") && count == 4)
                return link_forget(arguments[3]);
        if ((string_equals(verb, "allow") || string_equals(verb, "deny")) &&
            count >= 5)
                return link_grant_verb(arguments[3], string_equals(verb, "allow"),
                                  arguments + 4, count - 4);
        if (string_equals(verb, "shell") && count == 4)
                return link_client_run(arguments[3], LINK_KIND_SHELL, null, 0);
        if (string_equals(verb, "run") && count >= 5)
                return link_client_run(arguments[3], LINK_KIND_RUN,
                                       arguments + 4, count - 4);
        if (string_equals(verb, "push") && count == 6)
                return link_client_run(arguments[3], LINK_KIND_PUSH,
                                       arguments + 4, 2);
        if (string_equals(verb, "pull") && count == 6)
                return link_client_run(arguments[3], LINK_KIND_PULL,
                                       arguments + 4, 2);
        if (string_equals(verb, "log") && count == 4)
                return link_client_run(arguments[3], LINK_KIND_LOG, null, 0);

        return link_usage();
}

/*
        The machine process's side, once a loop: the listener runs while the
        switch is on and something is not already running it, restarted after
        one second, then two, four and on to a minute if it keeps dying; and
        stopped when the switch is off. Whoever holds the lock is the
        listener, whoever started it -- `link on`, this, or a person running
        `link serve` by hand.
*/
static p64 link_keep_next;
static p64 link_keep_wait = 1;

static fn link_keep(void)
{
        p64 now = system_clock_ns(HOST_CLOCK_BOOTTIME) / 1000000000ull;
        p8 word[16];
        bipolar owner;

        //      No switch at all is nobody's decision: a listener somebody ran
        //      by hand is left alone. Off is somebody's.
        host_read_text(LINK_SWITCH_PATH, word, sizeof word);
        if (!string_equals((string_address)word, "on"))
        {
                if (string_equals((string_address)word, "off") &&
                    link_lock_owner() > 0)
                        (void)link_lock_signal(15);
                return;
        }

        owner = link_lock_owner();

        if (owner > 0)
        {
                link_keep_wait = 1;
                return;
        }

        if (now < link_keep_next)
                return;

        link_keep_next = now + link_keep_wait;
        if (link_keep_wait < 60)
                link_keep_wait *= 2;

        {
                bipolar child = system_fork();

                if (!child)
                {
                        string_address words[] = {"moonwater", "link", "serve",
                                                  null};

                        (void)system_call(syscall(setsid));
                        (void)system_call_3(syscall(close_range), 3, ~0u, 0);
                        (void)shell_exec_file((string_address) "/proc/self/exe",
                                              words, 3, file_environment_all());
                        system_call_1(syscall(exit), 127);
                }
        }
}

#endif // WATERLINK_COMMAND_INCLUDED
