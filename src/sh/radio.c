/*
        Wireless and bluetooth, as moonwater verbs.

        Secrets stay on /root so an image update does not take the password
        with it. /ip watch still owns the address: this only joins the radio
        and says which link to prefer when both have carrier.
*/

#include "../net/nl80211.c"

#define RADIO_SSID_MOST 32
#define RADIO_PASS_MOST 63
#define RADIO_WIFI_MOST 16
#define RADIO_RFKILL_WLAN 1
#define RADIO_RFKILL_BLUETOOTH 2
#define RADIO_RFKILL_CHANGE_ALL 3
#define RADIO_LOCK_PATH NET_STATE_DIR "/radio.lock"
#define RADIO_LOCK_EX 2
#define RADIO_LOCK_NB 4
#define RADIO_LOCK_UN 8

typedef struct
{
        p8 ssid[RADIO_SSID_MOST + 1];
        p8 pass[RADIO_PASS_MOST + 1];
        p8 ssid_length;
        p8 pass_length;
} radio_network;

static fn radio_net_wake(void)
{
        bipolar handle;
        p8 one = '1';

        host_state_ready();
        system_call_4(syscall(mknodat), AT_FDCWD,
                      (positive)(string_address)NET_WAKE_PATH, S_IFIFO | 0600, 0);
        handle = system_open_at(AT_FDCWD, NET_WAKE_PATH,
                                FILE_READ_WRITE | O_NONBLOCK | O_CLOEXEC);
        if (handle < 0)
                return;

        system_write_all((positive)handle, address_of one, 1);
        system_close(handle);
}

/* One word and its newline over a state file. The room is a timezone name's
   room, because the clock's words come through here too. */
static bipolar radio_write_word(string_address path, string_address word)
{
        p8 line[96];

        string_copy_bounded(line, word, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        return host_write_file(path, line, string_length(line), 0644, true);
}

static bool radio_word_is(string_address path, string_address word)
{
        p8 text[16];

        if (host_read_text(path, text, sizeof(text)) < 0)
                return false;
        return string_equals(text, word);
}

static bipolar radio_lock(bool wait)
{
        bipolar handle;
        bipolar locked;

        host_state_ready();
        handle = system_open_at_mode(AT_FDCWD, RADIO_LOCK_PATH,
                                     FILE_READ_WRITE | FILE_CREATE | O_CLOEXEC,
                                     0600);
        if (handle < 0)
                return handle;

        locked = system_call_2(syscall(flock), (positive)handle,
                               wait ? RADIO_LOCK_EX
                                    : (RADIO_LOCK_EX | RADIO_LOCK_NB));
        if (locked < 0)
        {
                system_close(handle);
                return locked;
        }

        return handle;
}

static fn radio_unlock(bipolar handle)
{
        if (handle < 0)
                return;

        system_call_2(syscall(flock), (positive)handle, RADIO_LOCK_UN);
        system_close(handle);
}

static bipolar radio_rfkill(p8 type, bool block)
{
        ul_rfkill_event event = {
            .index = 0,
            .type = type,
            .operation = RADIO_RFKILL_CHANGE_ALL,
            .soft = block,
        };
        bipolar handle = system_open_at(AT_FDCWD, "/dev/rfkill",
                                        O_WRONLY | O_CLOEXEC | O_NONBLOCK);

        if (handle < 0)
                return handle;

        if (system_write_all((positive)handle, address_of event,
                             sizeof(event)) != sizeof(event))
        {
                system_close(handle);
                return -1;
        }

        system_close(handle);
        return 0;
}

static bool radio_text_plain(string_address text, positive length)
{
        positive at;

        for (at = 0; at < length; at++)
                if (text[at] < 32)
                        return false;
        return true;
}

static bool radio_line_has(p8 address_to text, positive got, string_address want)
{
        positive at = 0;
        positive want_length = string_length(want);

        while (at < got)
        {
                positive start = at;

                while (at < got && text[at] != '\n')
                        at++;
                if (at - start == want_length &&
                    !memory_compare(text + start, want, want_length))
                        return true;
                if (at < got)
                        at++;
        }

        return false;
}

static positive radio_wifi_load(radio_network address_to into, positive room)
{
        p8 text[8192];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_WIFI_LIST, text,
                                         sizeof(text));
        positive count = 0;
        positive at = 0;
        bool want_ssid = true;

        memory_fill(into, 0, sizeof(radio_network) * room);
        if (got <= 0)
                return 0;

        while (at < (positive)got && count < room)
        {
                positive start = at;
                positive length;

                while (at < (positive)got && text[at] != '\n')
                        at++;
                length = at - start;
                if (at < (positive)got)
                        at++;

                if (want_ssid)
                {
                        if (!length)
                                continue;
                        if (length > RADIO_SSID_MOST ||
                            !radio_text_plain((string_address)(text + start), length))
                        {
                                want_ssid = false;
                                continue;
                        }
                        memory_copy(into[count].ssid, text + start, length);
                        into[count].ssid[length] = end;
                        into[count].ssid_length = (p8)length;
                        into[count].pass[0] = end;
                        into[count].pass_length = 0;
                        want_ssid = false;
                        continue;
                }

                if (into[count].ssid_length)
                {
                        if (length &&
                            (length > RADIO_PASS_MOST ||
                             !radio_text_plain((string_address)(text + start),
                                               length)))
                        {
                                memory_fill(address_of into[count], 0,
                                            sizeof(into[count]));
                        }
                        else
                        {
                                memory_copy(into[count].pass, text + start, length);
                                into[count].pass[length] = end;
                                into[count].pass_length = (p8)length;
                                count++;
                        }
                }
                want_ssid = true;
        }

        if (!want_ssid && count < room && into[count].ssid_length)
                count++;

        return count;
}

static bipolar radio_wifi_save(radio_network address_to networks, positive count)
{
        p8 text[8192];
        positive used = 0;
        positive at;

        for (at = 0; at < count; at++)
        {
                if (used + networks[at].ssid_length + networks[at].pass_length +
                        2 >=
                    sizeof(text))
                        return -1;
                memory_copy(text + used, networks[at].ssid,
                            networks[at].ssid_length);
                used += networks[at].ssid_length;
                text[used++] = '\n';
                memory_copy(text + used, networks[at].pass,
                            networks[at].pass_length);
                used += networks[at].pass_length;
                text[used++] = '\n';
        }

        return host_write_file(NET_WIFI_LIST, text, used, 0600, true);
}

static bipolar radio_wifi_join(string_address ssid, string_address pass)
{
        p8 pmk[32];
        bipolar failed = -19;
        bool secured = pass && pass[0];
        positive ssid_length = string_length(ssid);
        p64 started;

        if (!ssid_length || ssid_length > RADIO_SSID_MOST)
                return -22;

        if (secured && !wifi_psk((p8 address_to)ssid, ssid_length,
                                 (p8 address_to)pass, string_length(pass), pmk))
                return -22;

        started = system_clock_ns(HOST_CLOCK_BOOTTIME);
        for (;;)
        {
                failed = nl80211_join((p8 address_to)ssid, ssid_length,
                                      secured ? pmk : null);
                if (failed != -19)
                        break;
                if (system_clock_ns(HOST_CLOCK_BOOTTIME) - started >= 8000000000)
                        break;
                host_pause(200000000);
        }

        crypto_forget(pmk, sizeof(pmk));
        return failed;
}

static bipolar radio_wifi_leave(void)
{
        nl80211 session;
        nl80211_iface iface;
        bipolar failed;

        failed = nl80211_open(address_of session);
        if (failed < 0)
                return failed;

        failed = nl80211_interface(address_of session, address_of iface);
        if (!failed)
                failed = nl80211_disconnect(address_of session, iface.index);

        nl80211_close(address_of session);
        return failed;
}

static b32 radio_wifi_bring(bool say)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count = radio_wifi_load(networks, RADIO_WIFI_MOST);
        positive at;
        bipolar failed = 0;
        bool joined = false;

        radio_write_word(NET_WIFI_POWER, "on");
        radio_rfkill(RADIO_RFKILL_WLAN, false);

        if (!say && nl80211_associated())
        {
                radio_net_wake();
                crypto_forget(networks, sizeof(networks));
                return 0;
        }

        for (at = 0; at < count; at++)
        {
                failed = radio_wifi_join((string_address)networks[at].ssid,
                                         (string_address)networks[at].pass);
                if (!failed)
                {
                        joined = true;
                        if (say)
                        {
                                string_format(log, host_label "wifi joined %s\n",
                                              (string_address)networks[at].ssid);
                                log_flush();
                        }
                        break;
                }
                if (failed == -19)
                        break;
        }

        radio_net_wake();
        crypto_forget(networks, sizeof(networks));

        if (!count)
        {
                if (say)
                {
                        string_format(log, host_label "wifi on\n");
                        log_flush();
                }
                return 0;
        }

        if (joined)
                return 0;

        if (say)
                return failed == -19
                           ? host_refuse("no wireless interface%s\n", "")
                           : failed == -110
                                 ? host_refuse("the network did not associate%s\n",
                                               "")
                                 : failed == -111
                                       ? host_refuse("the network refused the join%s\n",
                                                     "")
                                       : host_fail("wifi", failed ? failed : -1);
        return 1;
}

static b32 radio_wifi_on(bool say)
{
        bipolar lock = radio_lock(true);
        b32 result;

        if (lock < 0)
                return say ? host_fail("wifi", lock) : 1;
        result = radio_wifi_bring(say);
        radio_unlock(lock);
        return result;
}

static b32 radio_wifi_off(bool say)
{
        bipolar lock = radio_lock(true);

        if (lock < 0)
                return say ? host_fail("wifi", lock) : 1;
        radio_write_word(NET_WIFI_POWER, "off");
        radio_wifi_leave();
        radio_rfkill(RADIO_RFKILL_WLAN, true);
        radio_net_wake();
        radio_unlock(lock);
        if (say)
        {
                string_format(log, host_label "wifi off\n");
                log_flush();
        }
        return 0;
}

static b32 radio_wifi_add(string_address ssid, string_address pass)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count;
        positive at;
        positive ssid_length = string_length(ssid);
        positive pass_length = pass ? string_length(pass) : 0;

        if (!ssid_length || ssid_length > RADIO_SSID_MOST)
                return host_refuse("that network name is empty or too long%s\n",
                                   "");
        if (pass_length > RADIO_PASS_MOST)
                return host_refuse("that password is too long%s\n", "");
        if (pass_length && pass_length < 8 && pass_length != 64)
                return host_refuse("a WPA password is 8 to 63 characters%s\n",
                                   "");
        if (!radio_text_plain(ssid, ssid_length) ||
            (pass_length && !radio_text_plain(pass, pass_length)))
                return host_refuse("that network name cannot be stored%s\n", "");

        count = radio_wifi_load(networks, RADIO_WIFI_MOST);

        for (at = 0; at < count; at++)
                if (string_equals((string_address)networks[at].ssid, ssid))
                        break;

        if (at == count)
        {
                if (count == RADIO_WIFI_MOST)
                {
                        crypto_forget(networks, sizeof(networks));
                        return host_refuse("too many saved networks%s\n", "");
                }
                count++;
        }

        memory_fill(networks[at].ssid, 0, sizeof(networks[at].ssid));
        memory_copy(networks[at].ssid, ssid, ssid_length);
        networks[at].ssid[ssid_length] = end;
        networks[at].ssid_length = (p8)ssid_length;
        memory_fill(networks[at].pass, 0, sizeof(networks[at].pass));
        if (pass_length)
                memory_copy(networks[at].pass, pass, pass_length);
        networks[at].pass[pass_length] = end;
        networks[at].pass_length = (p8)pass_length;

        if (radio_wifi_save(networks, count) < 0)
        {
                crypto_forget(networks, sizeof(networks));
                return host_fail("wifi", -1);
        }

        radio_write_word(NET_WIFI_POWER, "on");
        radio_rfkill(RADIO_RFKILL_WLAN, false);

        {
                bipolar lock = radio_lock(true);
                bipolar failed;

                if (lock < 0)
                {
                        crypto_forget(networks, sizeof(networks));
                        return host_fail("wifi", lock);
                }
                failed = radio_wifi_join(ssid, pass);
                radio_unlock(lock);

                radio_net_wake();
                crypto_forget(networks, sizeof(networks));
                if (failed < 0)
                        return failed == -19
                                   ? host_refuse("saved, but there is no "
                                                 "wireless interface%s\n",
                                                 "")
                                   : failed == -110
                                         ? host_refuse("saved, but the network "
                                                       "did not associate%s\n",
                                                       "")
                                         : failed == -111
                                               ? host_refuse("saved, but the "
                                                             "network refused "
                                                             "the join%s\n",
                                                             "")
                                               : host_fail("wifi", failed);
                if (pass && pass[0])
                        crypto_forget((address_any)pass, string_length(pass));
        }

        string_format(log, host_label "wifi joined %s\n", ssid);
        log_flush();
        return 0;
}

static b32 radio_wifi_status(void)
{
        radio_network networks[RADIO_WIFI_MOST];
        positive count = radio_wifi_load(networks, RADIO_WIFI_MOST);
        positive at;
        bool off = radio_word_is(NET_WIFI_POWER, "off");

        string_format(log, host_label "wifi %s\n",
                      off ? (string_address) "off" : (string_address) "on");
        for (at = 0; at < count; at++)
                string_format(log, host_label "  %s\n",
                              (string_address)networks[at].ssid);
        log_flush();
        crypto_forget(networks, sizeof(networks));
        return 0;
}

/* The remembered word and the rfkill switch, set together. say is for the
   person who asked; restoring the machine's own choice stays quiet. */
static b32 radio_bluetooth_power(bool on, bool say)
{
        string_address word = on ? (string_address)"on" : (string_address)"off";

        radio_write_word(NET_BLUETOOTH_POWER, word);
        radio_rfkill(RADIO_RFKILL_BLUETOOTH, !on);
        if (say)
        {
                string_format(log, host_label "bluetooth %s\n", word);
                log_flush();
        }
        return 0;
}

static b32 radio_bluetooth_add(string_address identity)
{
        p8 text[4096];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_BLUETOOTH_LIST, text,
                                         sizeof(text));
        p8 line[320];
        positive used;

        if (!identity[0] || string_length(identity) > 128)
                return host_refuse("that bluetooth name is empty or too long%s\n",
                                   "");
        if (!radio_text_plain(identity, string_length(identity)))
                return host_refuse("that bluetooth name cannot be stored%s\n", "");

        if (got < 0)
        {
                text[0] = end;
                got = 0;
        }

        if (!(got > 0 && radio_line_has(text, (positive)got, identity)))
        {
                string_copy_bounded(line, identity, sizeof(line));
                string_append_bounded(line, "\n", sizeof(line));
                used = (positive)got;
                if (used + string_length(line) >= sizeof(text))
                        return host_refuse("too many saved bluetooth devices%s\n",
                                           "");
                memory_copy(text + used, line, string_length(line));
                used += string_length(line);
                if (host_write_file(NET_BLUETOOTH_LIST, text, used, 0644,
                                    true) < 0)
                        return host_fail("bluetooth", -1);
        }

        radio_bluetooth_power(true, false);
        string_format(log, host_label "bluetooth remembered %s\n", identity);
        log_flush();
        return 0;
}

static b32 radio_bluetooth_status(void)
{
        p8 text[4096];
        bipolar got = file_slurp_once_at(AT_FDCWD, NET_BLUETOOTH_LIST, text,
                                         sizeof(text));
        bool off = radio_word_is(NET_BLUETOOTH_POWER, "off");
        positive at = 0;

        string_format(log, host_label "bluetooth %s\n",
                      off ? (string_address) "off" : (string_address) "on");
        if (got > 0)
                while (at < (positive)got)
                {
                        positive start = at;

                        while (at < (positive)got && text[at] != '\n')
                                at++;
                        if (at > start)
                        {
                                p8 name[129];
                                positive length = at - start;

                                if (length > 128)
                                        length = 128;
                                memory_copy(name, text + start, length);
                                name[length] = end;
                                string_format(log, host_label "  %s\n",
                                              (string_address)name);
                        }
                        if (at < (positive)got)
                                at++;
                }
        log_flush();
        return 0;
}

static string_address radio_internet_word(void)
{
        return net_internet_prefer() == NETLINK_PREFER_WIFI
                   ? (string_address) "wifi"
                   : (string_address) "wired";
}

static b32 radio_internet_set(string_address which)
{
        p8 line[8];

        if (!string_equals(which, "wired") && !string_equals(which, "wifi"))
                return host_usage();

        string_copy_bounded(line, which, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        host_state_ready();
        if (host_write_text(NET_INTERNET_RUN, line) < 0)
                return host_fail("internet", -1);
        if (host_write_file(NET_INTERNET_ROOT, line, string_length(line),
                            0644, true) < 0)
                return host_fail("internet", -1);
        radio_net_wake();

        string_format(log, host_label "internet prefers %s\n", which);
        log_flush();
        return 0;
}

static b32 radio_internet_status(void)
{
        string_format(log, host_label "internet prefers %s\n",
                      radio_internet_word());
        log_flush();
        return 0;
}

static fn radio_internet_copy(void)
{
        p8 text[16];
        p8 have[16];
        p8 line[8];

        if (host_read_text(NET_INTERNET_ROOT, text, sizeof(text)) < 0)
                return;
        if (host_read_text(NET_INTERNET_RUN, have, sizeof(have)) >= 0 &&
            string_equals(have, text))
                return;

        string_copy_bounded(line, text, sizeof(line));
        string_append_bounded(line, "\n", sizeof(line));
        host_state_ready();
        if (host_write_text(NET_INTERNET_RUN, line) >= 0)
                radio_net_wake();
}

static bool radio_wifi_wanted(void)
{
        radio_network networks[1];
        bool wanted;

        if (radio_word_is(NET_WIFI_POWER, "off"))
                return false;
        if (radio_word_is(NET_WIFI_POWER, "on"))
                return true;
        wanted = radio_wifi_load(networks, 1) != 0;
        crypto_forget(networks, sizeof(networks));
        return wanted;
}

static fn radio_reap(void)
{
        positive status = 0;

        while (system_call_4(syscall(wait4), (positive)-1,
                             (positive)address_of status, 1, 0) > 0)
                ;
}

static fn radio_wifi_keep(void)
{
        bipolar lock;
        bipolar child;

        radio_rfkill(RADIO_RFKILL_WLAN, false);
        if (nl80211_associated())
                return;

        lock = radio_lock(false);
        if (lock < 0)
                return;

        child = system_fork();
        if (child < 0)
        {
                radio_unlock(lock);
                return;
        }
        if (child)
        {
                system_close(lock);
                return;
        }

        radio_wifi_bring(false);
        radio_unlock(lock);
        system_call_1(syscall(exit), 0);
}

static fn radio_restore(void)
{
        radio_internet_copy();

        if (radio_word_is(NET_WIFI_POWER, "off"))
                radio_wifi_off(false);
        else if (radio_wifi_wanted())
                radio_wifi_on(false);

        if (radio_word_is(NET_BLUETOOTH_POWER, "off"))
                radio_bluetooth_power(false, false);
        else if (radio_word_is(NET_BLUETOOTH_POWER, "on"))
                radio_bluetooth_power(true, false);
}

static fn radio_recover(void)
{
        p8 verdict[HOST_NAME_ROOM + 16];

        radio_reap();
        if (host_read_text(HOST_VERDICT, verdict, sizeof(verdict)) >= 0 &&
            host_starts(verdict, "ask "))
                return;

        radio_internet_copy();

        if (radio_wifi_wanted())
                radio_wifi_keep();

        if (radio_word_is(NET_BLUETOOTH_POWER, "on"))
                radio_rfkill(RADIO_RFKILL_BLUETOOTH, false);
}

static b32 host_radio(string_address address_to arguments, positive count)
{
        string_address verb = arguments[1];
        string_address word = count > 2 ? arguments[2] : null;
        bool mutate;

        if (string_equals(verb, "priority"))
        {
                if (count == 2)
                        return radio_internet_status();
                if (!string_equals(word, "internet"))
                        return host_usage();
                if (count == 3)
                        return radio_internet_status();
                if (count != 4)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return radio_internet_set(arguments[3]);
        }

        mutate = count > 2;
        if (mutate && !bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");

        if (string_equals(verb, "wifi"))
        {
                if (count < 3)
                        return radio_wifi_status();
                if (string_equals(word, "on") && count == 3)
                        return radio_wifi_on(true);
                if (string_equals(word, "off") && count == 3)
                        return radio_wifi_off(true);
                if (string_equals(word, "add") && count >= 4 && count <= 5)
                {
                        p8 pass[RADIO_PASS_MOST + 1];
                        string_address secret = count == 5 ? arguments[4]
                                                           : (string_address)"";
                        positive length = string_length(secret);
                        b32 result;

                        if (length > RADIO_PASS_MOST)
                                return host_refuse("that password is too long%s\n",
                                                   "");
                        memory_copy(pass, secret, length);
                        pass[length] = end;
                        if (count == 5)
                                crypto_forget(arguments[4], length);
                        result = radio_wifi_add(arguments[3], pass);
                        crypto_forget(pass, sizeof(pass));
                        return result;
                }
                return host_usage();
        }

        if (count < 3)
                return radio_bluetooth_status();
        if (string_equals(word, "on") && count == 3)
                return radio_bluetooth_power(true, true);
        if (string_equals(word, "off") && count == 3)
                return radio_bluetooth_power(false, true);
        if (string_equals(word, "add") && count == 4)
                return radio_bluetooth_add(arguments[3]);
        return host_usage();
}
