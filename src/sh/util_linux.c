/*
        Small util-linux process and I/O policy programs.

        These replace their process after applying policy, exactly like the
        upstream utilities. They share the file applets' option scanner,
        PATH/environment handoff and diagnostics instead of growing another
        command-line or exec layer.
*/

#define UL_TIOCSCTTY 0x540e
#define UL_TIOCSPGRP 0x5410
#define UL_SIGNAL_BLOCK 0
#define UL_SIGNAL_SET_MASK 2
#define UL_SIGNAL_TTOU 22

#define UL_IOPRIO_PROCESS 1
#define UL_IOPRIO_PGRP 2
#define UL_IOPRIO_USER 3
#define UL_IOPRIO_SHIFT 13
#define UL_IOPRIO_DATA_MASK ((1 << UL_IOPRIO_SHIFT) - 1)

static COLD b32 ul_usage(string_address program, string_address syntax)
{
        string_format(log, "Usage: %s %s\n", program, syntax);
        log_flush();
        return 0;
}

static COLD b32 ul_bad_usage(string_address program, string_address message)
{
        string_format(file_fail, "%s: %s\n", program, message);
        return 1;
}

static bool ul_size_number(string_address address_to text, positive base,
                           positive address_to value)
{
        string_address at = address_to text;
        positive got = 0;
        bool any = false;

        while (1)
        {
                p8 byte = string_get(at);
                positive digit;

                if (byte >= '0' && byte <= '9')
                        digit = byte - '0';
                else if (byte >= 'a' && byte <= 'f')
                        digit = byte - 'a' + 10;
                else if (byte >= 'A' && byte <= 'F')
                        digit = byte - 'A' + 10;
                else
                        break;

                positive scaled;

                if (digit >= base ||
                    __builtin_mul_overflow(got, base, address_of scaled) ||
                    __builtin_add_overflow(scaled, digit, address_of got))
                        return false;

                at++;
                any = true;
        }

        if (!any)
                return false;

        address_to text = at;
        address_to value = got;
        return true;
}

static bool ul_unsigned(string_address text, positive maximum,
                        positive address_to value)
{
        string_address at = text;
        positive got;

        while (byte_is_space(string_get(at)))
                at++;
        if (string_is(at, '+'))
                at++;
        else if (string_is(at, '-'))
                return false;

        if (!ul_size_number(address_of at, 10, address_of got) ||
            string_get(at) || got > maximum)
                return false;

        address_to value = got;
        return true;
}

/* The exact strtosize grammar shared by util-linux range options. */
static bool ul_size(string_address text, positive address_to value)
{
        string_address at = text;
        positive whole;
        positive fraction = 0;
        positive fraction_zeros = 0;
        positive base = 10;
        positive power;
        bool fractional = false;

        while (byte_is_space(string_get(at)))
                at++;
        if (string_is(at, '-'))
                return false;
        if (string_is(at, '+'))
                at++;

        if (string_is(at, '0'))
        {
                if (byte_to_upper(string_get(at + 1)) == 'X')
                {
                        base = 16;
                        at += 2;
                }
                else
                        base = 8;
        }

        if (!ul_size_number(address_of at, base, address_of whole))
                return false;

        if (string_is(at, '.'))
        {
                fractional = true;
                at++;
                while (string_is(at, '0'))
                {
                        fraction_zeros++;
                        at++;
                }
                if (byte_is_digit(string_get(at)) &&
                    !ul_size_number(address_of at, 10, address_of fraction))
                        return false;
        }

        p8 suffix = string_get(at++);

        if (!suffix)
                return !fractional && (address_to value = whole, true);

        power = file_size_power(suffix, true);

        if (!power || power > 8)
                return false;

        base = 1024;
        if (string_is(at, 'i') &&
            byte_to_upper(string_get(at + 1)) == 'B' &&
            !string_get(at + 2))
                at += 2;
        else if (byte_to_upper(string_get(at)) == 'B' && !string_get(at + 1))
        {
                base = 1000;
                at++;
        }
        else if (string_get(at))
                return false;

        positive scale = 1;
        for (; power; power--)
        {
                if (whole > positive_max / base)
                        return false;
                whole *= base;
                if (fraction)
                {
                        if (scale > positive_max / base)
                                return false;
                        scale *= base;
                }
        }

        if (fraction)
        {
                positive divisor = 10;
                positive place = 1;

                while (divisor < fraction)
                {
                        if (divisor <= positive_max / 10)
                                divisor *= 10;
                        else
                                fraction /= 10;
                }
                while (fraction_zeros--)
                {
                        if (divisor <= positive_max / 10)
                                divisor *= 10;
                        else
                                fraction /= 10;
                }

                do
                {
                        positive digit = fraction % 10;
                        positive represented = divisor / place;

                        fraction /= 10;
                        if (digit)
                        {
                                positive add = scale / (represented / digit);

                                if (whole > positive_max - add)
                                        return false;
                                whole += add;
                        }
                        if (fraction)
                        {
                                if (place > positive_max / 10)
                                        return false;
                                place *= 10;
                        }
                } while (fraction);
        }

        address_to value = whole;
        return true;
}

static b32 ul_exec(positive first, string_address program)
{
        positive count = (positive)program_argument_count();
        string_address address_to words;
        bipolar answer;

        if (first >= count)
                return ul_bad_usage(program, "no command specified");

        words = program_argument_list() + first;
        log_flush();
        answer = file_exec_path_try(words);
        string_format(file_fail, "%s: %s: %s\n", program, words[0],
                      file_reason(answer));
        return answer == -ERROR_NO_ENTRY ? 127 : 126;
}

static bool ul_meta(file_taking address_to taking, string_address syntax,
                    b32 address_to answer)
{
        if (taking->flags & FILE_FLAG('h'))
        {
                address_to answer = ul_usage(taking->program, syntax);
                return true;
        }

        if (taking->flags & FILE_FLAG('V'))
        {
                string_format(log, "%s from dawning-kit\n", taking->program);
                log_flush();
                address_to answer = 0;
                return true;
        }

        return false;
}

static const file_long ul_setsid_longs[] = {
    {(string_address)"ctty", 'c'}, {(string_address)"fork", 'f'},
    {(string_address)"wait", 'w'}, {(string_address)"help", 'h'},
    {(string_address)"version", 'V'}, {null, 0},
};

static b32 util_linux_setsid()
{
        file_taking taking = {
            .program = (string_address)"setsid",
            .allowed = (string_address)"cfwVh",
            .valued = (string_address)"",
            .longs = ul_setsid_longs,
        };
        b32 answer;
        bipolar pid;
        bool waiting;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] program [argument ...]", address_of answer))
                return answer;
        if (taking.first >= (positive)program_argument_count())
                return ul_bad_usage("setsid", "no command specified");

        waiting = (taking.flags & FILE_FLAG('w')) != 0;
        pid = system_call_1(syscall(getpid), 0);

        if ((taking.flags & FILE_FLAG('f')) ||
            system_call_1(syscall(getpgid), 0) == pid)
        {
                positive status = 0;
                bipolar child;

                log_flush();
                child = system_call_2(syscall(clone), SIGCHLD, 0);
                if (child < 0)
                {
                        string_format(file_fail, "setsid: fork: %s\n",
                                      file_reason(child));
                        return 1;
                }
                if (child > 0)
                {
                        if (!waiting)
                                return 0;
                        if (system_wait4_retry(child, address_of status, 0,
                                              null) < 0)
                                return ul_bad_usage("setsid", "wait failed");
                        return wait_status_code_base(status, 0);
                }
        }

        answer = (b32)system_call(syscall(setsid));
        if (answer < 0)
        {
                string_format(file_fail, "setsid: setsid failed: %s\n",
                              file_reason(answer));
                return 1;
        }

        if ((taking.flags & FILE_FLAG('c')) &&
            system_call_3(syscall(ioctl), 0, UL_TIOCSCTTY, 1) < 0)
                return ul_bad_usage("setsid",
                                    "failed to set the controlling terminal");

        return ul_exec(taking.first, "setsid");
}

static const file_long ul_setpgid_longs[] = {
    {(string_address)"foreground", 'f'}, {(string_address)"help", 'h'},
    {(string_address)"version", 'V'}, {null, 0},
};

static b32 util_linux_setpgid()
{
        file_taking taking = {
            .program = (string_address)"setpgid",
            .allowed = (string_address)"fVh",
            .valued = (string_address)"",
            .longs = ul_setpgid_longs,
        };
        b32 answer;
        bipolar changed;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] program [argument ...]", address_of answer))
                return answer;
        if (taking.first >= (positive)program_argument_count())
                return ul_bad_usage("setpgid", "no command specified");

        changed = system_call_2(syscall(setpgid), 0, 0);
        if (changed < 0)
        {
                string_format(file_fail, "setpgid: setpgid failed: %s\n",
                              file_reason(changed));
                return 1;
        }

        if (taking.flags & FILE_FLAG('f'))
        {
                bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                                (positive)"/dev/tty",
                                                FILE_READ | O_CLOEXEC, 0);

                /* Upstream deliberately ignores an absent controlling tty. */
                if (handle >= 0)
                {
                        positive blocked = (positive)1 << (UL_SIGNAL_TTOU - 1);
                        positive old = 0;
                        bipolar group = system_call_1(syscall(getpgid), 0);

                        if (system_call_4(syscall(rt_sigprocmask),
                                          UL_SIGNAL_BLOCK,
                                          (positive)address_of blocked,
                                          (positive)address_of old, 8) < 0 ||
                            group < 0 ||
                            system_call_3(syscall(ioctl), handle, UL_TIOCSPGRP,
                                          (positive)address_of group) < 0)
                        {
                                system_call_1(syscall(close), handle);
                                return ul_bad_usage("setpgid",
                                                    "cannot set foreground process group");
                        }

                        system_call_4(syscall(rt_sigprocmask),
                                      UL_SIGNAL_SET_MASK,
                                      (positive)address_of old, 0, 8);
                        system_call_1(syscall(close), handle);
                }
        }

        return ul_exec(taking.first, "setpgid");
}

static const file_long ul_fadvise_longs[] = {
    {(string_address)"advice", 'a'}, {(string_address)"fd", 'd'},
    {(string_address)"length", 'l'}, {(string_address)"offset", 'o'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static PURE b32 ul_fadvise_kind(string_address name)
{
        static string_address names[] = {
            "normal", "sequential", "random", "noreuse", "willneeded",
            "dontneed",
        };
        static p8 values[] = {0, 2, 1, 5, 3, 4};
        positive found = string_table_find(name, names, sizeof(names[0]),
                                           sizeof(names) / sizeof(names[0]));

        return found < sizeof(values) ? values[found] : -1;
}

static b32 util_linux_fadvise()
{
        file_taking taking = {
            .program = (string_address)"fadvise",
            .allowed = (string_address)"adloVh",
            .valued = (string_address)"adlo",
            .longs = ul_fadvise_longs,
        };
        positive offset = 0;
        positive length = 0;
        bipolar handle = -1;
        b32 advice = 4;
        b32 answer;
        bool close_handle = false;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] file | --fd descriptor", address_of answer))
                return answer;

        if (file_option_value(address_of taking, 'a'))
        {
                advice = ul_fadvise_kind(
                    file_option_value(address_of taking, 'a'));
                if (advice < 0)
                        return ul_bad_usage("fadvise", "invalid advice argument");
        }

        if ((file_option_value(address_of taking, 'l') &&
             !ul_size(file_option_value(address_of taking, 'l'),
                      address_of length)) ||
            (file_option_value(address_of taking, 'o') &&
             !ul_size(file_option_value(address_of taking, 'o'),
                      address_of offset)))
                return ul_bad_usage("fadvise", "invalid range");

        if (file_option_value(address_of taking, 'd'))
        {
                positive descriptor;

                if (!ul_unsigned(file_option_value(address_of taking, 'd'),
                                 b32_max,
                                 address_of descriptor))
                        return ul_bad_usage("fadvise", "invalid fd argument");
                if (taking.first < (positive)program_argument_count())
                        return ul_bad_usage(
                            "fadvise", "specify either descriptor or file");
                handle = (bipolar)descriptor;
        }
        else
        {
                if (taking.first >= (positive)program_argument_count())
                        return ul_bad_usage("fadvise", "no file specified");
                if (taking.first + 1 !=
                    (positive)program_argument_count())
                        return ul_bad_usage("fadvise", "too many files");

                handle = system_call_4(
                    syscall(openat), AT_FDCWD,
                    (positive)program_argument((b32)taking.first), FILE_READ,
                    0);
                if (handle < 0)
                {
                        string_format(file_fail, "fadvise: %s: %s\n",
                                      program_argument((b32)taking.first),
                                      file_reason(handle));
                        return 1;
                }
                close_handle = true;
        }

        answer = (b32)system_call_4(syscall(fadvise64), handle, offset,
                                    length, (positive)advice);
        if (close_handle)
                system_call_1(syscall(close), handle);

        if (answer < 0)
        {
                string_format(file_fail, "fadvise: failed to advise: %s\n",
                              file_reason(answer));
                return 1;
        }

        return 0;
}

static const file_long ul_ionice_longs[] = {
    {(string_address)"class", 'c'}, {(string_address)"classdata", 'n'},
    {(string_address)"pid", 'p'}, {(string_address)"pgid", 'P'},
    {(string_address)"uid", 'u'}, {(string_address)"ignore", 't'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static string_address ul_ionice_classes[] = {
    "none", "realtime", "best-effort", "idle",
};
static p8 ul_ionice_class_lengths[] = {4, 8, 11, 4};
static p8 ul_ionice_identity;

static bool ul_ionice_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter != 'p' && letter != 'P' && letter != 'u')
                return true;
        if (ul_ionice_identity)
        {
                ul_bad_usage("ionice", "only one of pid, pgid or uid is allowed");
                return false;
        }

        ul_ionice_identity = letter;
        return true;
}

static PURE b32 ul_ionice_class(string_address text)
{
        positive numeric;
        positive length = string_length(text);

        if (ul_unsigned(text, b32_max, address_of numeric))
                return (b32)numeric;

        for (positive at = 0;
             at < sizeof(ul_ionice_classes) / sizeof(ul_ionice_classes[0]); at++)
                if (length == ul_ionice_class_lengths[at] &&
                    !memory_compare_ascii_case(text, ul_ionice_classes[at], length))
                        return (b32)at;

        return -1;
}

static b32 ul_ionice_id(string_address text, string_address kind,
                        b32 address_to value)
{
        positive got;

        if (!ul_unsigned(text, b32_max, address_of got))
        {
                string_format(file_fail, "ionice: invalid %s: %s\n", kind,
                              text);
                return 1;
        }

        address_to value = (b32)got;
        return 0;
}

static b32 ul_ionice_get(b32 which, b32 id)
{
        bipolar raw = system_call_2(syscall(ioprio_get), (positive)which,
                                    (positive)id);
        b32 class;
        b32 data;

        if (raw < 0)
        {
                string_format(file_fail, "ionice: ioprio_get failed: %s\n",
                              file_reason(raw));
                return 1;
        }

        class = (b32)((positive)raw >> UL_IOPRIO_SHIFT);
        data = (b32)((positive)raw & UL_IOPRIO_DATA_MASK);
        string_address name = class >= 0 && class < 4
                                  ? ul_ionice_classes[class]
                                  : (string_address)"unknown";

        if (class == 3)
                string_format(log, "%s\n", name);
        else
                string_format(log, "%s: prio %b\n", name, (bipolar)data);

        return 0;
}

static b32 ul_ionice_set(b32 which, b32 id, b32 class, b32 data,
                         bool tolerant)
{
        positive priority = ((positive)(p32)class << UL_IOPRIO_SHIFT) |
                            ((positive)(p32)data & UL_IOPRIO_DATA_MASK);
        bipolar answer = system_call_3(syscall(ioprio_set), (positive)which,
                                       (positive)id, priority);

        if (answer >= 0 || tolerant)
                return 0;

        string_format(file_fail, "ionice: ioprio_set failed: %s\n",
                      file_reason(answer));
        return 1;
}

static b32 util_linux_ionice()
{
        file_taking taking = {
            .program = (string_address)"ionice",
            .allowed = (string_address)"cnpPutVh",
            .valued = (string_address)"cnpPu",
            .longs = ul_ionice_longs,
            .seen = ul_ionice_seen,
        };
        positive count = (positive)program_argument_count();
        b32 class = 2;
        b32 data = 4;
        b32 which = 0;
        b32 id = 0;
        b32 answer;
        b32 setting = 0;
        string_address id_kind = null;
        bool tolerant;

        ul_ionice_identity = 0;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] [-p pid ... | command]", address_of answer))
                return answer;

        if (file_option_value(address_of taking, 'c'))
        {
                class = ul_ionice_class(
                    file_option_value(address_of taking, 'c'));
                if (class < 0)
                        return ul_bad_usage("ionice",
                                            "unknown scheduling class");
                setting |= 2;
        }

        if (file_option_value(address_of taking, 'n'))
        {
                positive parsed;

                if (!ul_unsigned(file_option_value(address_of taking, 'n'),
                                 b32_max, address_of parsed))
                        return ul_bad_usage("ionice", "invalid class data");
                data = (b32)parsed;
                setting |= 1;
        }

        if (ul_ionice_identity)
        {
                which = ul_ionice_identity == 'p' ? UL_IOPRIO_PROCESS
                        : ul_ionice_identity == 'P' ? UL_IOPRIO_PGRP
                                                   : UL_IOPRIO_USER;
                id_kind = ul_ionice_identity == 'p' ? "PID"
                          : ul_ionice_identity == 'P' ? "PGID"
                                                     : "UID";
                answer = ul_ionice_id(
                    file_option_value(address_of taking, ul_ionice_identity),
                    id_kind,
                    address_of id);
        }
        else
                answer = 0;

        if (answer)
                return answer;

        tolerant = (taking.flags & FILE_FLAG('t')) != 0;
        if (class == 0)
                data = 0;
        else if (class == 3)
                data = 7;

        if (!setting && !which && taking.first == count)
                return ul_ionice_get(UL_IOPRIO_PROCESS, 0);

        if (which)
        {
                answer = setting
                             ? ul_ionice_set(which, id, class, data, tolerant)
                             : ul_ionice_get(which, id);

                for (positive at = taking.first; at < count && !answer; at++)
                {
                        if (ul_ionice_id(program_argument((b32)at), id_kind,
                                         address_of id))
                                return 1;
                        answer = setting
                                     ? ul_ionice_set(which, id, class, data,
                                                     tolerant)
                                     : ul_ionice_get(which, id);
                }

                log_flush();
                return answer;
        }

        if (taking.first < count)
        {
                answer = ul_ionice_set(UL_IOPRIO_PROCESS, 0, class, data,
                                       tolerant);
                return answer ? answer : ul_exec(taking.first, "ionice");
        }

        return ul_bad_usage("ionice", "bad usage");
}
