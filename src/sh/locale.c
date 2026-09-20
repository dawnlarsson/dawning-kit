/*
        Timezone, NTP and keyboard layout, as moonwater verbs.

        Choices live on /root so an image update keeps them. The machine
        starts NTP itself: restore forks the first query before init, and the
        wait loop keeps walking servers until the clock is set. Five samples
        keep the lowest delay unless /root/ntp.filter says off. The kernel
        adds that offset with adjtimex; a kiss-o-death drops the server.

        The forked query is reaped with wait4, not asked after with kill.
        A pid that has exited but not been waited for is still a pid, so
        kill(pid, 0) answers zero for a zombie exactly as it does for a
        live child: the poll would see its first query running for ever,
        never retry a boot that failed for want of a network, and never
        poll again. wait4 is the only call that distinguishes the two.

        NOTHING BELOW HAS BEEN SEEN TO SET A CLOCK

        Setting CLOCK_REALTIME needs a machine it is acceptable to
        disturb, and no machine in reach was one, so neither the step nor
        the slew has ever been run against a kernel that carried it out.
        What has been checked is what gets asked for: every ADJ_ and STA_
        value here against uapi/linux/timex.h, every timex word index
        against the struct's own offsets, and the choice between stepping
        and slewing against crafted offsets either side of the threshold
        in the machine lane. Take that as the request being right, not as
        the request having been granted.

        The reason to be careful about the difference is in the constants
        below. ADJ_SETOFFSET was 0x80, which is ADJ_TAI, for as long as
        this file has had it -- so no correction this program ever
        computed reached the clock. It survived because adjtimex answers a
        wrong mode the same way it answers a right one: with the clock
        state, a non-negative number the caller reads as success. It also
        cleared STA_UNSYNC on the way past, so the machine went on to
        report itself synchronised. Anyone adding a mode here should know
        that a non-negative return proves the call was accepted and
        nothing else, and that there is no test that can tell you
        otherwise, because nothing unprivileged can ask the kernel to
        demonstrate which mode a bit meant.
*/

#include "../net/sntp.c"

#define LOCALE_ZONE_PATH "/root/timezone"
#define LOCALE_NTP_PATH "/root/ntp"
#define LOCALE_NTP_SERVER_PATH "/root/ntp.server"
#define LOCALE_NTP_FILTER_PATH "/root/ntp.filter"
#define LOCALE_KEYBOARD_PATH "/root/keyboard"
#define LOCALE_NTP_DEFAULT_SERVER "pool.ntp.org"
#define LOCALE_NTP_RETRY_LEAST 1
#define LOCALE_NTP_RETRY_MOST 8
#define LOCALE_NTP_AGAIN 1800
#define LOCALE_WAIT_NOHANG 1
#define LOCALE_NTP_RATE_AGAIN 300
#define LOCALE_NTP_EXIT_RATE 2
#define LOCALE_NTP_STEP_NS ((bipolar)128 * 1000000)
#define LOCALE_NTP_TIMECONST 6
#define LOCALE_TIMEX_OFFSET 1
#define LOCALE_TIMEX_CONSTANT 6
/*
        The kernel's own numbering, from uapi/linux/timex.h. ADJ_SETOFFSET
        is 0x0100. It was 0x80 here, which is ADJ_TAI: a request to set the
        TAI offset from the constant word, not to step the clock from the
        time words. The kernel read word 6, which is zero, wrote that as
        the system TAI offset, ignored the offset we had gone to such
        lengths to measure, and returned the clock state -- a number the
        caller reads as success. So every sample, every filter and every
        guard in sntp.c fed a call that could not set the clock, said it
        had, and cleared STA_UNSYNC on the way out so the machine reported
        itself synchronised.
*/
#define ADJ_OFFSET 0x0001
#define ADJ_FREQUENCY 0x0002
#define ADJ_MAXERROR 0x0004
#define ADJ_ESTERROR 0x0008
#define ADJ_STATUS 0x0010
#define ADJ_TIMECONST 0x0020
#define ADJ_SETOFFSET 0x0100
#define ADJ_NANO 0x2000
#define STA_PLL 0x0001
#define STA_UNSYNC 0x0040
#define STA_NANO 0x2000

static p64 locale_ntp_next;
static positive locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
static bipolar locale_ntp_child;

static fn locale_ntp_keep(void);

static fn locale_word(string_address path, p8 address_to into, positive room)
{
        bipolar got = host_read_text(path, into, room);
        positive length;

        if (got < 0)
        {
                into[0] = end;
                return;
        }
        length = string_length(into);
        while (length && (into[length - 1] == '\n' || into[length - 1] == '\r'))
                into[--length] = end;
}

static bool locale_switch_on(string_address path)
{
        p8 word[16];

        locale_word(path, word, sizeof(word));
        if (!word[0])
                return true;
        return string_equals(word, "on");
}

static bool locale_ntp_wanted(void)
{
        return locale_switch_on(LOCALE_NTP_PATH);
}

static bool locale_ntp_filter_wanted(void)
{
        return locale_switch_on(LOCALE_NTP_FILTER_PATH);
}

static bool locale_clock_synced(void)
{
        return logger_clock_synced(null);
}

static bool locale_zone_ok(string_address name)
{
        positive at;

        if (!name || !name[0])
                return false;
        if (clock_zone_posix(name))
                return true;
        for (at = 0; name[at]; at++)
                if (name[at] >= '0' && name[at] <= '9')
                        return true;
        return false;
}

static b32 locale_zone_status(void)
{
        p8 zone[80];
        p64 now[2] = {0, 0};
        tm broken;
        time_t stamp;
        p8 when[40];

        locale_word(LOCALE_ZONE_PATH, zone, sizeof(zone));
        if (!zone[0])
                string_copy_bounded(zone, "UTC", sizeof(zone));
        system_call_2(syscall(clock_gettime), CLOCK_REALTIME, (positive)now);
        stamp = (time_t)now[0];
        tzset();
        if (!localtime_r(address_of stamp, address_of broken))
                return host_fail("timezone", -1);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", address_of broken);
        string_format(log, host_label "timezone %s\n", zone);
        string_format(log, "  local %s %s\n", when,
                      (string_address)broken.tm_zone);
        log_flush();
        return 0;
}

static b32 locale_zone_set(string_address name)
{
        if (!locale_zone_ok(name) || !radio_text_plain(name, string_length(name)))
                return host_refuse("unknown timezone %s\n", name);
        if (radio_write_word(LOCALE_ZONE_PATH, name) < 0)
                return host_fail("timezone", -1);
        tzset();
        string_format(log, host_label "timezone %s\n", name);
        log_flush();
        return 0;
}

static fn locale_clock_mark_synced(void)
{
        positive words[LOGGER_TIMEX_WORDS] = {0};

        words[0] = ADJ_STATUS;
        words[LOGGER_TIMEX_STATUS] = STA_PLL;
        system_call_1(syscall(adjtimex), (positive)words);
}

/*
        A correction applied once and then left alone is only right at
        the moment it lands. What carries the clock between polls is a
        crystal, and LOCALE_NTP_AGAIN is half an hour: ten parts per
        million, which is an ordinary one, is eighteen milliseconds of
        drift by the next query. That is three orders of magnitude past
        every other error on this path put together, and no amount of
        care measuring the offset touches any of it. Stepping and then
        free-running for 1800 seconds spends the whole measurement in
        the first instant and then throws it away.

        The kernel keeps a phase-locked loop for exactly this, and it
        keeps its frequency estimate across our polls -- which is what
        this program needs, because the query runs in a forked child
        that exits, so nothing held in memory survives to the next one.
        Handing the offset to that loop with ADJ_OFFSET and STA_PLL lets
        the kernel both steer the clock and learn how fast it runs; a
        poll interval longer than MINSEC puts it in the frequency-locked
        regime, which is the one that estimates rate from samples as far
        apart as ours.

        A step is still right when the clock is far out. Slewing never
        moves time backwards, which is what a log, a build and a file
        timestamp all want, but the kernel slews at a bounded rate, so a
        large offset would take longer to walk off than the gap between
        polls. The split is at 128 ms, where ntpd puts it.

        A step also cancels any slew still in progress, with an
        ADJ_OFFSET of zero in the same request: the pending phase
        adjustment was computed against a clock this request is about to
        move, and applying both would correct twice.
*/
static CONST bool locale_ntp_wants_step(bipolar offset_ns)
{
        return offset_ns >= LOCALE_NTP_STEP_NS ||
               offset_ns <= -LOCALE_NTP_STEP_NS;
}

static fn locale_ntp_discipline_words(bipolar offset_ns, bipolar seconds,
                                      bipolar nanoseconds,
                                      positive address_to words)
{
        memory_zero(words, LOGGER_TIMEX_WORDS * sizeof(positive));
        words[LOGGER_TIMEX_STATUS] = STA_PLL;
        if (locale_ntp_wants_step(offset_ns))
        {
                words[0] = ADJ_SETOFFSET | ADJ_OFFSET | ADJ_NANO | ADJ_STATUS;
                words[LOCALE_TIMEX_OFFSET] = 0;
                words[LOGGER_TIMEX_TIME_SEC] = (positive)seconds;
                words[LOGGER_TIMEX_TIME_NSEC] = (positive)nanoseconds;
                return;
        }
        words[0] = ADJ_OFFSET | ADJ_TIMECONST | ADJ_NANO | ADJ_STATUS;
        words[LOCALE_TIMEX_OFFSET] = (positive)offset_ns;
        words[LOCALE_TIMEX_CONSTANT] = LOCALE_NTP_TIMECONST;
}

/*
        Nothing unprivileged can ask the kernel which mode a bit means,
        and a wrong one returns success, so the decision is checked here
        instead: what goes in the request for a given offset, rather than
        what the kernel does with it.
*/
static COLD bool locale_discipline_ok(void)
{
        positive words[LOGGER_TIMEX_WORDS];
        positive at;
        static const struct
        {
                bipolar offset_ns;
                bool step;
        } discipline_case[] = {
            {0, false},
            {1000000, false},
            {-1000000, false},
            {LOCALE_NTP_STEP_NS - 1, false},
            {-(LOCALE_NTP_STEP_NS - 1), false},
            {LOCALE_NTP_STEP_NS, true},
            {-LOCALE_NTP_STEP_NS, true},
            {(bipolar)86400 * 1000000000, true},
            {-(bipolar)86400 * 1000000000, true},
        };

        for (at = 0; at < array_count(discipline_case); at++)
        {
                bipolar offset = discipline_case[at].offset_ns;
                bipolar sec = 0;
                bipolar nsec = 0;

                sntp_split_offset(offset, address_of sec, address_of nsec);
                locale_ntp_discipline_words(offset, sec, nsec, words);

                if (locale_ntp_wants_step(offset) != discipline_case[at].step)
                        return false;
                /* the loop is enabled either way, and the clock counts as
                   set either way, so STA_UNSYNC never survives a reply */
                if (words[LOGGER_TIMEX_STATUS] != STA_PLL)
                        return false;
                if (words[0] & ADJ_STATUS ? false : true)
                        return false;
                if (discipline_case[at].step)
                {
                        /* a step carries the time, cancels any slew, and
                           has no business setting a loop time constant */
                        if (!(words[0] & ADJ_SETOFFSET) ||
                            words[0] & ADJ_TIMECONST ||
                            words[LOCALE_TIMEX_OFFSET] != 0 ||
                            (bipolar)words[LOGGER_TIMEX_TIME_SEC] != sec ||
                            (bipolar)words[LOGGER_TIMEX_TIME_NSEC] != nsec)
                                return false;
                }
                else
                {
                        /* a slew hands the offset to the loop and never
                           steps, so time does not go backwards */
                        if (words[0] & ADJ_SETOFFSET ||
                            !(words[0] & ADJ_TIMECONST) ||
                            (bipolar)words[LOCALE_TIMEX_OFFSET] != offset ||
                            words[LOCALE_TIMEX_CONSTANT] !=
                                LOCALE_NTP_TIMECONST ||
                            words[LOGGER_TIMEX_TIME_SEC] ||
                            words[LOGGER_TIMEX_TIME_NSEC])
                                return false;
                }
                if (!(words[0] & ADJ_NANO) || !(words[0] & ADJ_OFFSET))
                        return false;
        }
        return true;
}

static const char locale_ntp_fallback[][24] = {
        LOCALE_NTP_DEFAULT_SERVER,
        "time.google.com",
        "time.cloudflare.com",
        "216.239.35.0",
        "216.239.35.4",
        "162.159.200.1",
        "162.159.200.123",
};

static bipolar locale_ntp_apply_offset(bipolar offset_ns)
{
        bipolar now;
        bipolar target = 0;
        bipolar sec = 0;
        bipolar nsec = 0;
        positive words[LOGGER_TIMEX_WORDS];
        p64 stamp[2];
        bipolar failed;

        now = sntp_now_ns();
        if (now < 0)
                return now;
        if (!sntp_target_ok(now, offset_ns, address_of target))
                return SNTP_MALFORMED;

        sntp_split_offset(offset_ns, address_of sec, address_of nsec);
        locale_ntp_discipline_words(offset_ns, sec, nsec, words);
        failed = system_call_1(syscall(adjtimex), (positive)words);
        if_common (failed >= 0)
                return 0;

        now = sntp_now_ns();
        if (now < 0)
                return now;
        if (!sntp_target_ok(now, offset_ns, address_of target))
                return SNTP_MALFORMED;
        stamp[0] = (p64)(target / (bipolar)SNTP_NANOSECONDS);
        stamp[1] = (p64)(target % (bipolar)SNTP_NANOSECONDS);
        failed = system_call_2(syscall(clock_settime), CLOCK_REALTIME,
                               (positive)stamp);
        if (failed < 0)
        {
                stamp[1] = stamp[1] / 1000;
                failed = system_call_2(syscall(settimeofday), (positive)stamp, 0);
        }
        if (failed < 0)
                return failed;
        locale_clock_mark_synced();
        return 0;
}

static bipolar locale_ntp_one(string_address server, bool filter, bool tight)
{
        bipolar offset_ns = 0;
        bipolar failed;

        if (!server || !server[0] ||
            !radio_text_plain(server, string_length(server)))
                return SNTP_NO_SERVER;
        failed = sntp_query(server, filter, tight, address_of offset_ns);
        if (failed < 0)
                return failed;
        return locale_ntp_apply_offset(offset_ns);
}

/*
        A server answering RATE is telling us we ask too often. Walking to
        the next name and asking that one immediately is not an answer to
        it, and when the next name is another address of the same pool it
        is the complaint repeated. The verdict is carried out of the walk
        so a cycle that ended in nothing but rate limits waits properly
        instead of coming back in a second and doing it again.
*/
static bipolar locale_ntp_apply(void)
{
        p8 server[80];
        bipolar failed = SNTP_NO_SERVER;
        positive at;
        bool filter = locale_ntp_filter_wanted();
        bool tight = locale_clock_synced();
        bool rated = false;

        locale_word(LOCALE_NTP_SERVER_PATH, server, sizeof(server));
        if (server[0])
        {
                failed = locale_ntp_one((string_address)server, filter, tight);
                if (failed >= 0)
                        return 0;
                rated = failed == SNTP_RATE_LIMITED;
        }
        for (at = 0; at < array_count(locale_ntp_fallback); at++)
        {
                if (server[0] &&
                    string_equals((string_address)server,
                                  (string_address)locale_ntp_fallback[at]))
                        continue;
                failed = locale_ntp_one((string_address)locale_ntp_fallback[at],
                                        filter, tight);
                if (failed >= 0)
                        return 0;
                if (failed == SNTP_RATE_LIMITED)
                        rated = true;
        }
        return rated ? SNTP_RATE_LIMITED : failed;
}

static b32 locale_ntp_status(void)
{
        p8 server[80];
        bool wanted = locale_ntp_wanted();
        bool synced = locale_clock_synced();

        locale_word(LOCALE_NTP_SERVER_PATH, server, sizeof(server));
        if (!server[0])
                string_copy_bounded(server, LOCALE_NTP_DEFAULT_SERVER,
                                    sizeof(server));
        string_format(log, host_label "ntp %s, %s, filter %s, %s\n",
                      wanted ? "on" : "off", server,
                      locale_ntp_filter_wanted() ? "on" : "off",
                      synced ? "synchronised" : "waiting");
        log_flush();
        return 0;
}

static b32 locale_ntp_filter_status(void)
{
        string_format(log, host_label "ntp filter %s\n",
                      locale_ntp_filter_wanted() ? "on" : "off");
        log_flush();
        return 0;
}

static COLD b32 locale_ntp_filter_set(string_address word)
{
        if (!string_equals(word, "on") && !string_equals(word, "off"))
                return host_usage();
        if (radio_write_word(LOCALE_NTP_FILTER_PATH, word) < 0)
                return host_fail("ntp", -1);
        string_format(log, host_label "ntp filter %s\n", word);
        log_flush();
        return 0;
}

static b32 locale_ntp_set(string_address word)
{
        if (!string_equals(word, "on") && !string_equals(word, "off"))
                return host_usage();
        if (radio_write_word(LOCALE_NTP_PATH, word) < 0)
                return host_fail("ntp", -1);
        if (string_equals(word, "on"))
        {
                locale_ntp_next = 0;
                if (locale_ntp_apply() < 0)
                {
                        string_format(log, host_label "ntp on, waiting for a reply\n");
                        log_flush();
                        return 0;
                }
        }
        string_format(log, host_label "ntp %s\n", word);
        log_flush();
        return 0;
}

static const struct
{
        char name[8];
} locale_keyboards[] = {
        {"us"}, {"uk"}, {"gb"}, {"de"}, {"se"}, {"sv"}, {"no"}, {"nb"},
        {"dk"}, {"fi"}, {"fr"}, {"es"}, {"it"},
};

static bool locale_keyboard_ok(string_address name)
{
        positive at;

        for (at = 0; at < array_count(locale_keyboards); at++)
                if (string_equals(name, (string_address)locale_keyboards[at].name))
                        return true;
        return false;
}

static b32 locale_keyboard_live(string_address name)
{
        struct canvas_control control;

        memory_zero(address_of control, sizeof(control));
        control.request = SPARK_CANVAS_LAYOUT;
        if (name)
        {
                positive length = string_length(name);

                if (length >= sizeof(control.master_command))
                        return -22;
                memory_copy(control.master_command, name, length);
        }
        return host_spark_once(SPARK_IOCTL_CANVAS, address_of control, FILE_READ);
}

static b32 locale_keyboard_status(void)
{
        p8 name[16];
        struct canvas_control control;

        locale_word(LOCALE_KEYBOARD_PATH, name, sizeof(name));
        if (!name[0])
                string_copy_bounded(name, "us", sizeof(name));
        memory_zero(address_of control, sizeof(control));
        control.request = SPARK_CANVAS_LAYOUT;
        if (host_spark_once(SPARK_IOCTL_CANVAS, address_of control, FILE_READ) >= 0 &&
            control.master_command[0])
                string_copy_bounded(name, control.master_command, sizeof(name));
        string_format(log, host_label "keyboard %s\n", name);
        log_flush();
        return 0;
}

static b32 locale_keyboard_set(string_address name)
{
        if (!locale_keyboard_ok(name))
                return host_refuse("unknown keyboard layout %s\n", name);
        if (radio_write_word(LOCALE_KEYBOARD_PATH, name) < 0)
                return host_fail("keyboard", -1);
        (void)locale_keyboard_live(name);
        string_format(log, host_label "keyboard %s\n", name);
        log_flush();
        return 0;
}

static fn locale_restore(void)
{
        p8 zone[80];
        p8 keyboard[16];

        locale_word(LOCALE_ZONE_PATH, zone, sizeof(zone));
        if (zone[0])
                tzset();

        locale_word(LOCALE_KEYBOARD_PATH, keyboard, sizeof(keyboard));
        if (keyboard[0])
                locale_keyboard_live(keyboard);

        locale_ntp_next = 0;
        locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
        locale_ntp_child = 0;
        if (locale_ntp_wanted())
                locale_ntp_keep();
}

static fn locale_ntp_keep(void)
{
        p64 now = system_clock_ns(HOST_CLOCK_BOOTTIME);
        bipolar child;

        if (locale_ntp_child > 0)
        {
                positive status = 0;
                bipolar reaped = system_wait4_retry(locale_ntp_child,
                                                    address_of status,
                                                    LOCALE_WAIT_NOHANG, null);

                if (reaped == 0)
                        return;
                locale_ntp_child = 0;
                if (locale_clock_synced())
                {
                        locale_ntp_retry = LOCALE_NTP_RETRY_LEAST;
                        locale_ntp_next =
                            now + (p64)LOCALE_NTP_AGAIN * 1000000000ull;
                }
                else if (((status >> 8) & 0xff) == LOCALE_NTP_EXIT_RATE)
                {
                        locale_ntp_retry = LOCALE_NTP_RETRY_MOST;
                        locale_ntp_next =
                            now + (p64)LOCALE_NTP_RATE_AGAIN * 1000000000ull;
                }
                else
                {
                        locale_ntp_next =
                            now + (p64)locale_ntp_retry * 1000000000ull;
                        if (locale_ntp_retry < LOCALE_NTP_RETRY_MOST)
                                locale_ntp_retry *= 2;
                }
                return;
        }

        if (locale_ntp_next && now < locale_ntp_next)
                return;

        child = system_fork();
        if (child < 0)
                return;
        if (!child)
        {
                bipolar failed = locale_ntp_apply();

                system_call_1(syscall(exit),
                              failed >= 0
                                  ? 0
                                  : (failed == SNTP_RATE_LIMITED
                                         ? LOCALE_NTP_EXIT_RATE
                                         : 1));
        }
        locale_ntp_child = child;
}

static fn locale_recover(void)
{
        if (locale_ntp_wanted())
                locale_ntp_keep();
}

static b32 host_locale(string_address address_to arguments, positive count)
{
        string_address verb = arguments[1];
        string_address word = count > 2 ? arguments[2] : null;

        if (string_equals(verb, "timezone"))
        {
                if (count == 2)
                        return locale_zone_status();
                if (count != 3)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return locale_zone_set(word);
        }

        if (string_equals(verb, "ntp"))
        {
                if (count == 2)
                        return locale_ntp_status();
                if (string_equals(word, "filter"))
                {
                        if (count == 3)
                                return locale_ntp_filter_status();
                        if (count != 4)
                                return host_usage();
                        if (!bowl_is_root())
                                return host_refuse("%s needs root\n", "moonwater");
                        return locale_ntp_filter_set(arguments[3]);
                }
                if (count != 3)
                        return host_usage();
                if (!bowl_is_root())
                        return host_refuse("%s needs root\n", "moonwater");
                return locale_ntp_set(word);
        }

        if (count == 2)
                return locale_keyboard_status();
        if (count != 3)
                return host_usage();
        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");
        return locale_keyboard_set(word);
}
