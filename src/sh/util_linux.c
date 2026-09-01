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

#define UL_CLOCK_MONOTONIC 1
#define UL_LOCK_SHARED 1
#define UL_LOCK_EXCLUSIVE 2
#define UL_LOCK_NONBLOCK 4
#define UL_LOCK_UNLOCK 8
#define UL_ERROR_INTERRUPTED (-4)
#define UL_ERROR_AGAIN 11

#define UL_CPU_WORDS 1024
#define UL_CPU_BITS (UL_CPU_WORDS * positive_bits)

typedef b32 (*ul_task_action)(b32 pid, address_any context);

typedef struct
{
        p32 size;
        p32 policy;
        p64 flags;
        b32 nice;
        p32 priority;
        p64 runtime;
        p64 deadline;
        p64 period;
        p32 util_min;
        p32 util_max;
} ul_sched_attr;

typedef struct
{
        p64 soft;
        p64 hard;
} ul_limit_pair;

#define UL_LIMIT_INFINITE ((p64)0 - 1)

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

static bool ul_signed(string_address text, bipolar minimum, bipolar maximum,
                      bipolar address_to value)
{
        bipolar got;

        if (!nice_adjustment(text, address_of got) || got < minimum ||
            got > maximum)
                return false;

        address_to value = got;
        return true;
}

static bool ul_pid(string_address text, string_address program,
                   string_address kind, b32 address_to value)
{
        positive got;

        if (!ul_unsigned(text, b32_max, address_of got))
        {
                string_format(file_fail, "%s: invalid %s: %s\n", program,
                              kind, text);
                return false;
        }

        address_to value = (b32)got;
        return true;
}

/* taskset, chrt and uclampset all give -a the same meaning. */
static b32 ul_tasks(b32 pid, bool all, ul_task_action action,
                    address_any context)
{
        if (!all)
                return action(pid, context);

        p8 path[64] = "/proc/";
        positive at = 6;

        at += positive_into_string(path + at, (positive)(p32)pid);
        memory_copy_apart_end(path + at, "/task", 5);

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
                return 1;

        b32 failed = 0;
        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                positive task;

                if (!ul_unsigned((string_address)entry->d_name, b32_max,
                                 address_of task))
                        continue;

                if (action((b32)task, context))
                        failed = 1;
        }

        file_walk_close(address_of walk);
        return failed;
}

static PURE b32 ul_hex(p8 byte)
{
        if (byte >= '0' && byte <= '9')
                return byte - '0';
        byte = byte_to_lower(byte);
        return byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : -1;
}

static bool ul_cpu_mask(string_address text, positive address_to set)
{
        positive length = string_length(text);
        positive nibble = 0;
        bool any = false;

        memory_fill(set, 0, UL_CPU_WORDS * sizeof(*set));

        while (length && byte_is_space(string_get(text + length - 1)))
                length--;

        positive first = string_span(text, string_set_blanks);
        if (length >= first + 2 && string_is(text + first, '0') &&
            byte_to_lower(string_get(text + first + 1)) == 'x')
                first += 2;

        while (length > first)
        {
                p8 byte = string_get(text + --length);

                if (byte == ',')
                        continue;

                b32 digit = ul_hex(byte);
                if (digit < 0 || nibble >= UL_CPU_BITS / 4)
                        return false;

                set[nibble / (positive_bits / 4)] |=
                    (positive)(p32)digit << ((nibble % (positive_bits / 4)) * 4);
                nibble++;
                any = true;
        }

        return any;
}

static bool ul_cpu_list(string_address text, positive address_to set)
{
        string_address at = text;
        bool any = false;

        memory_fill(set, 0, UL_CPU_WORDS * sizeof(*set));

        while (string_get(at))
        {
                positive first;
                positive last;
                positive stride = 1;

                if (!ul_size_number(address_of at, 10, address_of first) ||
                    first >= UL_CPU_BITS)
                        return false;

                last = first;
                if (string_is(at, '-'))
                {
                        at++;
                        if (!ul_size_number(address_of at, 10, address_of last) ||
                            last < first || last >= UL_CPU_BITS)
                                return false;
                }

                if (string_is(at, ':'))
                {
                        at++;
                        if (!ul_size_number(address_of at, 10,
                                            address_of stride) ||
                            !stride)
                                return false;
                }

                for (positive cpu = first; cpu <= last;)
                {
                        set[cpu / positive_bits] |=
                            (positive)1 << (cpu % positive_bits);
                        any = true;

                        if (last - cpu < stride)
                                break;
                        cpu += stride;
                }

                if (!string_get(at))
                        break;
                if (!string_is(at, ','))
                        return false;
                at++;
        }

        return any;
}

static bool ul_cpu_set(string_address text, bool list,
                       positive address_to set)
{
        return list ? ul_cpu_list(text, set) : ul_cpu_mask(text, set);
}

static fn ul_cpu_mask_say(positive address_to set, positive bytes)
{
        static p8 digits[] = "0123456789abcdef";
        positive nibbles = bytes * 2;

        while (nibbles > 1)
        {
                positive at = nibbles - 1;
                positive digit = set[at / (positive_bits / 4)] >>
                                 ((at % (positive_bits / 4)) * 4) & 15;
                if (digit)
                        break;
                nibbles--;
        }

        for (positive left = nibbles; left; left--)
        {
                positive at = left - 1;
                positive digit = set[at / (positive_bits / 4)] >>
                                 ((at % (positive_bits / 4)) * 4) & 15;

                if (left != nibbles && !(left % 8))
                        log(",", 1);
                log(digits + digit, 1);
        }
}

static bool ul_cpu_has(positive address_to set, positive cpu)
{
        return (set[cpu / positive_bits] &
                ((positive)1 << (cpu % positive_bits))) != 0;
}

static fn ul_cpu_list_say(positive address_to set, positive bytes)
{
        bool comma = false;
        positive bits = bytes * 8;

        for (positive first = 0; first < bits; first++)
        {
                if (!ul_cpu_has(set, first))
                        continue;

                positive second = first + 1;
                while (second < bits && !ul_cpu_has(set, second))
                        second++;

                positive stride = second < bits ? second - first : 1;
                positive last = first;
                positive count = 1;

                while (last + stride < bits)
                {
                        positive next = last + stride;
                        positive between = last + 1;

                        while (between < next && !ul_cpu_has(set, between))
                                between++;
                        if (between < next || !ul_cpu_has(set, next))
                                break;

                        last = next;
                        count++;
                }

                if (count < 3)
                {
                        last = first;
                        count = 1;
                }

                if (comma)
                        log(",", 1);
                positive_to_string(log, first);

                if (count > 1)
                {
                        log("-", 1);
                        positive_to_string(log, last);
                        if (stride > 1)
                        {
                                log(":", 1);
                                positive_to_string(log, stride);
                        }
                }

                comma = true;
                first = last;
        }
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

static b32 ul_exec_words(string_address address_to words,
                         string_address program);

static b32 ul_exec(positive first, string_address program)
{
        positive count = (positive)program_argument_count();

        if (first >= count)
                return ul_bad_usage(program, "no command specified");

        return ul_exec_words(program_argument_list() + first, program);
}

static b32 ul_exec_words(string_address address_to words,
                         string_address program)
{
        log_flush();
        bipolar answer = file_exec_path_try(words);
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

// taskset ---------------------------------------------------------
typedef struct
{
        positive wanted[UL_CPU_WORDS];
        bool list;
        bool setting;
        bool report;
} ul_taskset_work;

static fn ul_taskset_say(b32 pid, string_address state, bool list,
                         positive address_to set, positive bytes)
{
        string_format(log, "pid %b's %s affinity %s: ", (bipolar)pid,
                      state, list ? (string_address)"list"
                                  : (string_address)"mask");
        if (list)
                ul_cpu_list_say(set, bytes);
        else
                ul_cpu_mask_say(set, bytes);
        log("\n", 1);
}

static b32 ul_taskset_one(b32 pid, address_any context)
{
        ul_taskset_work address_to work = context;
        positive current[UL_CPU_WORDS];
        bipolar used;

        if (work->report)
        {
                used = system_call_3(syscall(sched_getaffinity),
                                     (positive)(p32)pid, sizeof(current),
                                     (positive)current);
                if (used < 0)
                {
                        string_format(file_fail,
                                      "taskset: failed to get pid %b's affinity: %s\n",
                                      (bipolar)pid, file_reason(used));
                        return 1;
                }

                ul_taskset_say(pid, (string_address)"current", work->list,
                               current, (positive)used);
        }

        if (!work->setting)
                return 0;

        bipolar changed = system_call_3(syscall(sched_setaffinity),
                                        (positive)(p32)pid,
                                        sizeof(work->wanted),
                                        (positive)work->wanted);
        if (changed < 0)
        {
                string_format(file_fail,
                              "taskset: failed to set pid %b's affinity: %s\n",
                              (bipolar)pid, file_reason(changed));
                return 1;
        }

        if (work->report)
        {
                used = system_call_3(syscall(sched_getaffinity),
                                     (positive)(p32)pid, sizeof(current),
                                     (positive)current);
                if (used < 0)
                        return 1;
                ul_taskset_say(pid, (string_address)"new", work->list,
                               current, (positive)used);
        }

        return 0;
}

static const file_long ul_taskset_longs[] = {
    {(string_address)"all-tasks", 'a'},
    {(string_address)"pid", 'p'},
    {(string_address)"cpu-list", 'c'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static b32 util_linux_taskset()
{
        file_taking taking = {
            .program = (string_address)"taskset",
            .allowed = (string_address)"apcVh",
            .valued = (string_address)"",
            .longs = ul_taskset_longs,
        };
        positive count = (positive)program_argument_count();
        ul_taskset_work work = {.list = false, .setting = false, .report = true};
        b32 answer;
        b32 pid = 0;
        bool by_pid;
        bool all;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] [mask | cpu-list] [pid | command ...]",
                    address_of answer))
                return answer;

        by_pid = (taking.flags & FILE_FLAG('p')) != 0;
        all = (taking.flags & FILE_FLAG('a')) != 0;
        work.list = (taking.flags & FILE_FLAG('c')) != 0;

        if (by_pid)
        {
                positive operands = count - taking.first;

                if (operands != 1 && operands != 2)
                        return ul_bad_usage("taskset", "bad usage");

                work.setting = operands == 2;
                if (work.setting &&
                    !ul_cpu_set(program_argument((b32)taking.first), work.list,
                                work.wanted))
                        return ul_bad_usage("taskset",
                                            work.list
                                              ? "failed to parse CPU list"
                                              : "failed to parse CPU mask");

                if (!ul_pid(program_argument((b32)(count - 1)), "taskset",
                            "PID", address_of pid))
                        return 1;

                answer = ul_tasks(pid, all, ul_taskset_one, address_of work);
                log_flush();
                return answer;
        }

        if (all || taking.first + 1 >= count)
                return ul_bad_usage("taskset", "bad usage");

        if (!ul_cpu_set(program_argument((b32)taking.first), work.list,
                        work.wanted))
                return ul_bad_usage("taskset",
                                    work.list ? "failed to parse CPU list"
                                              : "failed to parse CPU mask");

        work.setting = true;
        work.report = false;
        if (ul_taskset_one(0, address_of work))
                return 1;

        return ul_exec(taking.first + 1, "taskset");
}

// renice ----------------------------------------------------------
#define UL_PRIO_PROCESS 0
#define UL_PRIO_PGRP 1
#define UL_PRIO_USER 2

static b32 ul_renice_one(string_address operand, b32 which,
                         bipolar priority, bool relative)
{
        positive id;

        if (which == UL_PRIO_USER &&
            !ul_unsigned(operand, b32_max, address_of id))
        {
                bipolar named = file_user_id(operand);
                if (named < 0)
                {
                        string_format(file_fail, "renice: unknown user %s\n",
                                      operand);
                        return 1;
                }
                id = (positive)named;
        }
        else if (!ul_unsigned(operand, b32_max, address_of id))
        {
                string_format(file_fail, "renice: bad value: %s\n", operand);
                return 1;
        }

        string_address kind = which == UL_PRIO_PROCESS
                                  ? (string_address)"process ID"
                              : which == UL_PRIO_PGRP
                                  ? (string_address)"process group ID"
                                  : (string_address)"user ID";
        bipolar raw = system_call_2(syscall(getpriority), (positive)which, id);

        if (raw < 0)
        {
                string_format(file_fail,
                              "renice: failed to get priority for %p (%s): %s\n",
                              id, kind, file_reason(raw));
                return 1;
        }

        bipolar old = 20 - raw;
        /* Upstream narrows the adjustment to int before adding it. */
        bipolar wanted = relative
            ? old + (bipolar)(b32)(p32)priority : priority;

        if (wanted < -20)
                wanted = -20;
        else if (wanted > 19)
                wanted = 19;

        bipolar changed = system_call_3(syscall(setpriority), (positive)which,
                                        id, (positive)wanted);
        if (changed < 0)
        {
                string_format(file_fail,
                              "renice: failed to set priority for %p (%s): %s\n",
                              id, kind, file_reason(changed));
                return 1;
        }

        raw = system_call_2(syscall(getpriority), (positive)which, id);
        bipolar now = raw < 0 ? wanted : 20 - raw;

        string_format(log, "%p (%s) old priority %b, new priority %b\n",
                      id, kind, old, now);
        return 0;
}

static bool ul_renice_long_value(string_address word, string_address name,
                                 string_address address_to value)
{
        positive length = string_length(name);

        if (!string_is(word, '-') || !string_is(word + 1, '-') ||
            memory_compare(word + 2, name, length))
                return false;

        if (string_is(word + 2 + length, '='))
        {
                address_to value = word + 3 + length;
                return true;
        }

        if (!string_get(word + 2 + length))
        {
                address_to value = null;
                return true;
        }

        return false;
}

static b32 util_linux_renice()
{
        positive count = (positive)program_argument_count();
        positive at = 1;
        bool relative = false;
        string_address priority_text;

        if (at >= count)
                return ul_bad_usage("renice", "not enough arguments");

        string_address first = program_argument((b32)at++);
        if (string_equals(first, "-h") || string_equals(first, "--help"))
                return ul_usage("renice", "priority [-p|-g|-u] ID ...");
        if (string_equals(first, "-V") || string_equals(first, "--version"))
        {
                string_format(log, "renice from dawning-kit\n");
                log_flush();
                return 0;
        }

        string_address value = null;
        bool short_priority = string_is(first, '-') && string_is(first + 1, 'n');
        bool long_priority =
            ul_renice_long_value(first, "priority", address_of value) ||
            ul_renice_long_value(first, "relative", address_of value);

        if (short_priority || long_priority)
        {
                relative = short_priority
                    ? file_environment("POSIXLY_CORRECT") != null
                    : string_get(first + 2) == 'r';
                priority_text = short_priority && string_get(first + 2)
                                    ? first + 2 : value;
                if (!priority_text && at < count)
                        priority_text = program_argument((b32)at++);
        }
        else
                priority_text = first;

        bipolar priority;
        if (!priority_text ||
            !ul_signed(priority_text, bipolar_min, bipolar_max,
                       address_of priority))
                return ul_bad_usage("renice", "invalid priority");

        b32 which = UL_PRIO_PROCESS;
        b32 failed = 0;
        bool identity_option = false;
        positive ids = 0;
        bool options = true;

        for (; at < count; at++)
        {
                string_address word = program_argument((b32)at);

                if (options && string_equals(word, "--"))
                {
                        options = false;
                        continue;
                }
                if (options &&
                    (string_equals(word, "-p") || string_equals(word, "--pid")))
                {
                        which = UL_PRIO_PROCESS;
                        identity_option = true;
                        continue;
                }
                if (options &&
                    (string_equals(word, "-g") || string_equals(word, "--pgrp")))
                {
                        which = UL_PRIO_PGRP;
                        identity_option = true;
                        continue;
                }
                if (options &&
                    (string_equals(word, "-u") || string_equals(word, "--user")))
                {
                        which = UL_PRIO_USER;
                        identity_option = true;
                        continue;
                }

                failed |= ul_renice_one(word, which, priority, relative);
                ids++;
        }

        log_flush();
        return ids ? failed : identity_option ? 0
                                              : ul_bad_usage("renice", "no process ID specified");
}

// prlimit ---------------------------------------------------------
static bipolar ul_prlimit(b32 pid, positive resource,
                          ul_limit_pair address_to in,
                          ul_limit_pair address_to out)
{
        return system_call_4(syscall(prlimit64), (positive)(p32)pid,
                             resource, (positive)in, (positive)out);
}

typedef struct
{
        string_address name;
        string_address description;
        string_address units;
        p8 letter;
        p8 resource;
} ul_resource;

static const ul_resource ul_resources[] = {
    {"AS", "address space limit", "bytes", 'v', 9},
    {"CORE", "max core file size", "bytes", 'c', 4},
    {"CPU", "CPU time", "seconds", 't', 0},
    {"DATA", "max data size", "bytes", 'd', 2},
    {"FSIZE", "max file size", "bytes", 'f', 1},
    {"LOCKS", "max number of file locks held", "locks", 'x', 10},
    {"MEMLOCK", "max locked-in-memory address space", "bytes", 'l', 8},
    {"MSGQUEUE", "max bytes in POSIX mqueues", "bytes", 'q', 12},
    {"NICE", "max nice prio allowed to raise", "", 'e', 13},
    {"NOFILE", "max number of open files", "files", 'n', 7},
    {"NPROC", "max number of processes", "processes", 'u', 6},
    {"RSS", "max resident set size", "bytes", 'm', 5},
    {"RTPRIO", "max real-time priority", "", 'r', 14},
    {"RTTIME", "timeout for real-time tasks", "microsecs", 'y', 15},
    {"SIGPENDING", "max number of pending signals", "signals", 'i', 11},
    {"STACK", "max stack size", "bytes", 's', 3},
};

#define UL_RESOURCES (sizeof(ul_resources) / sizeof(ul_resources[0]))

static const file_long ul_prlimit_longs[] = {
    {(string_address)"pid", 'p'}, {(string_address)"output", 'o'},
    {(string_address)"noheadings", 'H'}, {(string_address)"raw", 'R'},
    {(string_address)"verbose", 'z'},
    {(string_address)"core", 'c'}, {(string_address)"data", 'd'},
    {(string_address)"nice", 'e'}, {(string_address)"fsize", 'f'},
    {(string_address)"sigpending", 'i'}, {(string_address)"memlock", 'l'},
    {(string_address)"rss", 'm'}, {(string_address)"nofile", 'n'},
    {(string_address)"msgqueue", 'q'}, {(string_address)"rtprio", 'r'},
    {(string_address)"stack", 's'}, {(string_address)"cpu", 't'},
    {(string_address)"nproc", 'u'}, {(string_address)"as", 'v'},
    {(string_address)"locks", 'x'}, {(string_address)"rttime", 'y'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static bool ul_limit_value(string_address text, p64 current,
                           p64 address_to value)
{
        if (!text || !string_get(text))
        {
                address_to value = current;
                return true;
        }
        if (string_equals(text, "-1") || string_equals(text, "unlimited") ||
            string_equals(text, "infinity"))
        {
                address_to value = UL_LIMIT_INFINITE;
                return true;
        }

        positive got;
        if (!ul_unsigned(text, positive_max, address_of got))
                return false;
        address_to value = (p64)got;
        return true;
}

static bool ul_limit_parse(string_address text, ul_limit_pair current,
                           ul_limit_pair address_to out)
{
        if (!string_get(text))
                return false;

        string_address colon = string_first_of(text, ':');

        if (!colon)
        {
                if (!ul_limit_value(text, current.soft, address_of out->soft))
                        return false;
                out->hard = out->soft;
                return true;
        }

        p8 left[64];
        positive length = (positive)(colon - text);
        if (!length && !string_get(colon + 1))
                return false;
        if (length >= sizeof(left))
                return false;
        memory_copy_apart_end(left, text, length);

        return ul_limit_value(left, current.soft, address_of out->soft) &&
               ul_limit_value(colon + 1, current.hard, address_of out->hard);
}

enum
{
        UL_LIMIT_RESOURCE,
        UL_LIMIT_DESCRIPTION,
        UL_LIMIT_SOFT,
        UL_LIMIT_HARD,
        UL_LIMIT_UNITS,
        UL_LIMIT_COLUMNS
};

static string_address ul_limit_headers[] = {
    "RESOURCE", "DESCRIPTION", "SOFT", "HARD", "UNITS",
};

static bool ul_limit_columns(string_address text, p8 address_to columns,
                             positive address_to count)
{
        positive made = 0;

        while (string_get(text))
        {
                string_address comma = string_first_of(text, ',');
                positive length = comma ? (positive)(comma - text)
                                        : string_length(text);
                positive found = UL_LIMIT_COLUMNS;

                for (positive at = 0; at < UL_LIMIT_COLUMNS; at++)
                        if (length == string_length(ul_limit_headers[at]) &&
                            !memory_compare_ascii_case(text,
                                                       ul_limit_headers[at],
                                                       length))
                        {
                                found = at;
                                break;
                        }

                if (found == UL_LIMIT_COLUMNS || made == UL_LIMIT_COLUMNS)
                        return false;
                columns[made++] = (p8)found;
                text += length;
                if (!string_get(text))
                        break;
                text++;
        }

        address_to count = made;
        return made != 0;
}

static positive ul_limit_text(p64 value, p8 address_to into)
{
        if (value == UL_LIMIT_INFINITE)
        {
                memory_copy_apart_end(into, "unlimited", 9);
                return 9;
        }

        return positive_into_string(into, (positive)value);
}

static fn ul_limit_raw(string_address text)
{
        while (string_get(text))
        {
                if (string_is(text, ' '))
                        log("\\x20", 4);
                else
                        log(text, 1);
                text++;
        }
}

static fn ul_limit_field(p8 column, ul_resource const address_to resource,
                         ul_limit_pair pair, positive width, bool raw)
{
        p8 number[32];
        string_address text;

        if (column == UL_LIMIT_RESOURCE)
                text = resource->name;
        else if (column == UL_LIMIT_DESCRIPTION)
                text = resource->description;
        else if (column == UL_LIMIT_UNITS)
                text = resource->units;
        else
        {
                ul_limit_text(column == UL_LIMIT_SOFT ? pair.soft : pair.hard,
                              number);
                text = number;
        }

        if (raw)
                ul_limit_raw(text);
        else
                string_to_field(log, text, width, ' ',
                                column != UL_LIMIT_SOFT &&
                                column != UL_LIMIT_HARD);
}

static b32 util_linux_prlimit()
{
        file_taking taking = {
            .program = (string_address)"prlimit",
            .allowed = (string_address)"pocdefilmnqrstuvxyVh",
            .valued = (string_address)"po",
            .optional = (string_address)"cdefilmnqrstuvxy",
            .longs = ul_prlimit_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] [--resource[=limit]] [command ...]",
                    address_of answer))
                return answer;

        positive count = (positive)program_argument_count();
        b32 pid = 0;
        bool command = taking.first < count;

        if (file_option_value(address_of taking, 'p'))
        {
                if (command)
                        return ul_bad_usage("prlimit",
                                            "cannot specify a PID and a command");
                if (!ul_pid(file_option_value(address_of taking, 'p'),
                            "prlimit", "PID", address_of pid))
                        return 1;
        }

        bool selected = false;
        b32 failed = 0;

        for (positive at = 0; at < UL_RESOURCES; at++)
        {
                ul_resource const address_to resource = ul_resources + at;
                positive bit = FILE_FLAG(resource->letter);

                if (!(taking.flags & bit))
                        continue;
                selected = true;

                string_address value =
                    file_option_value(address_of taking, resource->letter);
                if (!value)
                        continue;
                ul_limit_pair old;
                ul_limit_pair made;
                bipolar got = ul_prlimit(pid, resource->resource, null,
                                         address_of old);
                if (got < 0 || !ul_limit_parse(value, old, address_of made))
                {
                        string_format(file_fail,
                                      "prlimit: failed to parse %s limit\n",
                                      resource->name);
                        failed = 1;
                        continue;
                }

                got = ul_prlimit(pid, resource->resource, address_of made, null);
                if (got < 0)
                {
                        string_format(file_fail, "prlimit: failed to set %s: %s\n",
                                      resource->name, file_reason(got));
                        failed = 1;
                        continue;
                }

                if ((taking.flags & FILE_FLAG('z')) && !command)
                {
                        b32 shown_pid = pid ? pid
                                            : (b32)system_call(syscall(getpid));
                        string_format(log, "New %s limit for pid %b: <",
                                      resource->name, (bipolar)shown_pid);
                        p8 text[32];
                        ul_limit_text(made.soft, text);
                        string_format(log, "%s:", text);
                        ul_limit_text(made.hard, text);
                        string_format(log, "%s>\n", text);
                }
        }

        if (failed)
        {
                log_flush();
                return 1;
        }

        if (command)
                return ul_exec(taking.first, "prlimit");

        p8 columns[UL_LIMIT_COLUMNS] = {
            UL_LIMIT_RESOURCE, UL_LIMIT_DESCRIPTION, UL_LIMIT_SOFT,
            UL_LIMIT_HARD, UL_LIMIT_UNITS,
        };
        positive column_count = UL_LIMIT_COLUMNS;
        if (file_option_value(address_of taking, 'o') &&
            !ul_limit_columns(file_option_value(address_of taking, 'o'),
                              columns, address_of column_count))
                return ul_bad_usage("prlimit", "unknown column");

        bool headings = !(taking.flags & FILE_FLAG('H'));
        bool raw = (taking.flags & FILE_FLAG('R')) != 0;
        positive widths[UL_LIMIT_COLUMNS] = {0, 0, 0, 0, 0};
        ul_limit_pair pairs[UL_RESOURCES];
        bool show[UL_RESOURCES];
        bool any_show = false;

        for (positive column = 0; column < UL_LIMIT_COLUMNS; column++)
                if (headings)
                        widths[column] = string_length(ul_limit_headers[column]);

        for (positive at = 0; at < UL_RESOURCES; at++)
        {
                ul_resource const address_to resource = ul_resources + at;
                show[at] = (!selected ||
                            (taking.flags & FILE_FLAG(resource->letter))) &&
                           !file_option_value(address_of taking,
                                              resource->letter);
                if (!show[at])
                        continue;

                bipolar got = ul_prlimit(pid, resource->resource, null,
                                         pairs + at);
                if (got < 0)
                {
                        string_format(file_fail, "prlimit: failed to get %s: %s\n",
                                      resource->name, file_reason(got));
                        show[at] = false;
                        failed = 1;
                        continue;
                }
                any_show = true;

                positive lengths[UL_LIMIT_COLUMNS];
                p8 number[32];
                lengths[UL_LIMIT_RESOURCE] = string_length(resource->name);
                lengths[UL_LIMIT_DESCRIPTION] =
                    string_length(resource->description);
                lengths[UL_LIMIT_SOFT] =
                    ul_limit_text(pairs[at].soft, number);
                lengths[UL_LIMIT_HARD] =
                    ul_limit_text(pairs[at].hard, number);
                lengths[UL_LIMIT_UNITS] = string_length(resource->units);

                for (positive column = 0; column < UL_LIMIT_COLUMNS; column++)
                        if (lengths[column] > widths[column])
                                widths[column] = lengths[column];
        }

        if (headings && any_show)
        {
                for (positive at = 0; at < column_count; at++)
                {
                        if (at)
                                log(" ", 1);
                        string_to_field(log, ul_limit_headers[columns[at]],
                                        raw || (at + 1 == column_count &&
                                                columns[at] != UL_LIMIT_SOFT &&
                                                columns[at] != UL_LIMIT_HARD)
                                            ? 0 : widths[columns[at]], ' ',
                                        columns[at] != UL_LIMIT_SOFT &&
                                        columns[at] != UL_LIMIT_HARD);
                }
                log("\n", 1);
        }

        for (positive at = 0; at < UL_RESOURCES; at++)
        {
                ul_resource const address_to resource = ul_resources + at;
                if (!show[at])
                        continue;

                for (positive field = 0; field < column_count; field++)
                {
                        if (field)
                                log(" ", 1);
                        ul_limit_field(columns[field], resource, pairs[at],
                                       raw || (field + 1 == column_count &&
                                               columns[field] != UL_LIMIT_SOFT &&
                                               columns[field] != UL_LIMIT_HARD)
                                           ? 0 : widths[columns[field]], raw);
                }
                log("\n", 1);
        }

        log_flush();
        return failed;
}

// chrt and uclampset ---------------------------------------------
#define UL_SCHED_RESET_ON_FORK 0x01
#define UL_SCHED_UTIL_MIN 0x20
#define UL_SCHED_UTIL_MAX 0x40

static bipolar ul_sched_get(b32 pid, ul_sched_attr address_to attr)
{
        memory_fill(attr, 0, sizeof(*attr));
        attr->size = sizeof(*attr);
        return system_call_4(syscall(sched_getattr), (positive)(p32)pid,
                             (positive)attr, sizeof(*attr), 0);
}

static bipolar ul_sched_set(b32 pid, ul_sched_attr address_to attr)
{
        attr->size = sizeof(*attr);
        return system_call_3(syscall(sched_setattr), (positive)(p32)pid,
                             (positive)attr, 0);
}

typedef struct
{
        string_address name;
        p8 option;
        p8 value;
} ul_policy;

static const ul_policy ul_policies[] = {
    {"SCHED_OTHER", 'o', 0}, {"SCHED_FIFO", 'f', 1},
    {"SCHED_RR", 'r', 2}, {"SCHED_BATCH", 'b', 3},
    {"SCHED_IDLE", 'i', 5}, {"SCHED_DEADLINE", 'd', 6},
    {"SCHED_EXT", 'e', 7},
};

#define UL_POLICIES (sizeof(ul_policies) / sizeof(ul_policies[0]))

static ul_policy const address_to ul_policy_option(p8 option)
{
        for (positive at = 0; at < UL_POLICIES; at++)
                if (ul_policies[at].option == option)
                        return ul_policies + at;
        return null;
}

static ul_policy const address_to ul_policy_value(p32 value)
{
        for (positive at = 0; at < UL_POLICIES; at++)
                if (ul_policies[at].value == value)
                        return ul_policies + at;
        return null;
}

static fn ul_chrt_report(b32 pid, string_address state,
                         ul_sched_attr address_to attr)
{
        ul_policy const address_to policy = ul_policy_value(attr->policy);

        string_format(log, "pid %b's %s scheduling policy: %s\n",
                      (bipolar)pid, state,
                      policy ? policy->name : (string_address)"SCHED_UNKNOWN");
        string_format(log, "pid %b's %s scheduling priority: %p\n",
                      (bipolar)pid, state, (positive)attr->priority);
        string_format(log, "pid %b's %s runtime parameter: %p\n",
                      (bipolar)pid, state, (positive)attr->runtime);

        if (attr->policy == 6)
        {
                string_format(log, "pid %b's %s deadline parameter: %p\n",
                              (bipolar)pid, state, (positive)attr->deadline);
                string_format(log, "pid %b's %s period parameter: %p\n",
                              (bipolar)pid, state, (positive)attr->period);
        }
}

typedef struct
{
        ul_sched_attr attr;
        bool setting;
        bool verbose;
} ul_chrt_work;

static b32 ul_chrt_one(b32 pid, address_any context)
{
        ul_chrt_work address_to work = context;
        ul_sched_attr current;

        if (!work->setting)
        {
                bipolar got = ul_sched_get(pid, address_of current);
                if (got < 0)
                {
                        string_format(file_fail,
                                      "chrt: failed to get pid %b's policy: %s\n",
                                      (bipolar)pid, file_reason(got));
                        return 1;
                }
                ul_chrt_report(pid, (string_address)"current",
                               address_of current);
                return 0;
        }

        if (work->verbose && ul_sched_get(pid, address_of current) >= 0)
                ul_chrt_report(pid, (string_address)"current",
                               address_of current);

        bipolar changed = ul_sched_set(pid, address_of work->attr);
        if (changed < 0)
        {
                string_format(file_fail,
                              "chrt: failed to set pid %b's policy: %s\n",
                              (bipolar)pid, file_reason(changed));
                return 1;
        }

        if (work->verbose && ul_sched_get(pid, address_of current) >= 0)
                ul_chrt_report(pid, (string_address)"new", address_of current);
        return 0;
}

static p8 ul_chrt_policy;
static const file_supersede ul_chrt_supersedes[] = {
    {(string_address)"bdefior", address_of ul_chrt_policy},
    {null, null},
};

static const file_long ul_chrt_longs[] = {
    {(string_address)"batch", 'b'}, {(string_address)"deadline", 'd'},
    {(string_address)"ext", 'e'}, {(string_address)"fifo", 'f'},
    {(string_address)"idle", 'i'}, {(string_address)"other", 'o'},
    {(string_address)"rr", 'r'}, {(string_address)"reset-on-fork", 'R'},
    {(string_address)"sched-runtime", 'T'},
    {(string_address)"sched-period", 'P'},
    {(string_address)"sched-deadline", 'D'},
    {(string_address)"all-tasks", 'a'}, {(string_address)"max", 'm'},
    {(string_address)"pid", 'p'}, {(string_address)"verbose", 'v'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static b32 ul_chrt_max()
{
        for (positive at = 0; at < UL_POLICIES; at++)
        {
                bipolar low = system_call_1(syscall(sched_get_priority_min),
                                            ul_policies[at].value);
                bipolar high = system_call_1(syscall(sched_get_priority_max),
                                             ul_policies[at].value);
                if (low < 0 || high < 0)
                        continue;

                string_format(log, "%s min/max priority\t: %b/%b\n",
                              ul_policies[at].name, low, high);
        }
        log_flush();
        return 0;
}

static b32 util_linux_chrt()
{
        file_taking taking = {
            .program = (string_address)"chrt",
            .allowed = (string_address)"bdefiorRTPDampvVh",
            .valued = (string_address)"TPD",
            .longs = ul_chrt_longs,
            .supersedes = ul_chrt_supersedes,
        };
        b32 answer;

        ul_chrt_policy = 0;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] [priority] command | -p [priority] PID",
                    address_of answer))
                return answer;
        if (taking.flags & FILE_FLAG('m'))
                return ul_chrt_max();

        positive count = (positive)program_argument_count();
        bool by_pid = (taking.flags & FILE_FLAG('p')) != 0;
        bool all = (taking.flags & FILE_FLAG('a')) != 0;
        ul_policy const address_to policy =
            ul_policy_option(ul_chrt_policy ? ul_chrt_policy : 'r');
        positive priority = 0;
        positive first = taking.first;
        bool priority_given = false;

        if (first < count && (!by_pid || count - first > 1) &&
            ul_unsigned(program_argument((b32)first), p32_max, address_of priority))
        {
                priority_given = true;
                first++;
        }

        if (by_pid)
        {
                if (first + 1 != count)
                        return ul_bad_usage("chrt", "bad usage");
        }
        else if (first >= count || all)
                return ul_bad_usage("chrt", "bad usage");

        b32 pid = 0;
        if (by_pid && !ul_pid(program_argument((b32)first), "chrt", "PID",
                              address_of pid))
                return 1;

        bool scheduling_option = ul_chrt_policy || priority_given ||
            file_option_value(address_of taking, 'T') ||
            file_option_value(address_of taking, 'P') ||
            file_option_value(address_of taking, 'D');

        if (by_pid && !scheduling_option)
        {
                ul_chrt_work query = {.setting = false, .verbose = true};
                answer = ul_tasks(pid, all, ul_chrt_one, address_of query);
                log_flush();
                return answer;
        }

        if ((policy->value == 1 || policy->value == 2) && !priority_given)
                return ul_bad_usage("chrt", "missing priority");

        ul_chrt_work work;
        memory_fill(address_of work, 0, sizeof(work));
        work.setting = true;
        work.verbose = (taking.flags & FILE_FLAG('v')) != 0;
        work.attr.size = sizeof(work.attr);
        work.attr.policy = policy->value;
        work.attr.priority = (p32)priority;
        work.attr.flags = (taking.flags & FILE_FLAG('R'))
                              ? UL_SCHED_RESET_ON_FORK
                              : 0;

        struct { p8 option; p64 address_to into; } parameters[] = {
            {'T', address_of work.attr.runtime},
            {'D', address_of work.attr.deadline},
            {'P', address_of work.attr.period},
        };
        for (positive at = 0; at < sizeof(parameters) / sizeof(parameters[0]); at++)
        {
                string_address value =
                    file_option_value(address_of taking, parameters[at].option);
                positive got;
                if (value && !ul_unsigned(value, positive_max, address_of got))
                        return ul_bad_usage("chrt", "invalid scheduling parameter");
                if (value)
                        address_to parameters[at].into = (p64)got;
        }

        if (ul_tasks(pid, all, ul_chrt_one, address_of work))
        {
                log_flush();
                return 1;
        }
        log_flush();
        return by_pid ? 0 : ul_exec(first, "chrt");
}

typedef struct
{
        bool setting;
        bool verbose;
        bool minimum;
        bool maximum;
        p32 min;
        p32 max;
        bool reset;
} ul_uclamp_work;

static fn ul_uclamp_name(b32 pid, p8 address_to name)
{
        p8 path[64] = "/proc/";
        positive at = 6;

        at += positive_into_string(path + at, (positive)(p32)pid);
        memory_copy_apart_end(path + at, "/comm", 5);
        bipolar got = file_slurp(path, name, FILE_NAME_MAX);
        if (got <= 0)
        {
                memory_copy_apart_end(name, "unknown", 7);
                return;
        }
        p8 address_to newline = (p8 address_to)memory_first_of(name, '\n',
                                                               (positive)got);
        if (newline)
                address_to newline = end;
}

static fn ul_uclamp_report(b32 pid, ul_sched_attr address_to attr)
{
        p8 name[FILE_NAME_MAX];
        ul_uclamp_name(pid, name);
        string_format(log, "%s (%b) util_clamp: min: %p max: %p\n", name,
                      (bipolar)pid, (positive)attr->util_min,
                      (positive)attr->util_max);
}

static b32 ul_uclamp_one(b32 pid, address_any context)
{
        ul_uclamp_work address_to work = context;
        ul_sched_attr attr;
        bipolar got = ul_sched_get(pid, address_of attr);

        if (got < 0)
        {
                string_format(file_fail,
                              "uclampset: failed to get pid %b's attributes: %s\n",
                              (bipolar)pid, file_reason(got));
                return 1;
        }

        if (work->setting)
        {
                if (work->minimum)
                {
                        attr.util_min = work->min;
                        attr.flags |= UL_SCHED_UTIL_MIN;
                }
                if (work->maximum)
                {
                        attr.util_max = work->max;
                        attr.flags |= UL_SCHED_UTIL_MAX;
                }
                if (work->reset)
                        attr.flags |= UL_SCHED_RESET_ON_FORK;

                got = ul_sched_set(pid, address_of attr);
                if (got < 0)
                {
                        string_format(file_fail,
                                      "uclampset: failed to set pid %b's attributes: %s\n",
                                      (bipolar)pid, file_reason(got));
                        return 1;
                }

                if (!work->verbose)
                        return 0;
                if (ul_sched_get(pid, address_of attr) < 0)
                        return 1;
        }

        ul_uclamp_report(pid, address_of attr);
        return 0;
}

static const file_long ul_uclamp_longs[] = {
    {(string_address)"all-tasks", 'a'}, {(string_address)"pid", 'p'},
    {(string_address)"system", 's'}, {(string_address)"reset-on-fork", 'R'},
    {(string_address)"verbose", 'v'}, {(string_address)"help", 'h'},
    {(string_address)"version", 'V'}, {null, 0},
};

static b32 util_linux_uclampset()
{
        file_taking taking = {
            .program = (string_address)"uclampset",
            .allowed = (string_address)"mMapsRvVh",
            .valued = (string_address)"mMp",
            .longs = ul_uclamp_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] --pid PID | command [argument ...] | --system",
                    address_of answer))
                return answer;

        ul_uclamp_work work = {
            .setting = false,
            .verbose = (taking.flags & FILE_FLAG('v')) != 0,
            .minimum = file_option_value(address_of taking, 'm') != null,
            .maximum = file_option_value(address_of taking, 'M') != null,
            .reset = (taking.flags & FILE_FLAG('R')) != 0,
        };
        struct { p8 option; p32 address_to into; } values[] = {
            {'m', address_of work.min}, {'M', address_of work.max},
        };
        for (positive at = 0; at < 2; at++)
        {
                string_address value = file_option_value(address_of taking,
                                                          values[at].option);
                bipolar got;
                if (value && !ul_signed(value, -1, 1024, address_of got))
                        return ul_bad_usage("uclampset",
                                            "utilization value must be -1..1024");
                if (value)
                        address_to values[at].into = (p32)got;
        }

        bool system = (taking.flags & FILE_FLAG('s')) != 0;
        string_address pid_text = file_option_value(address_of taking, 'p');
        bool all = (taking.flags & FILE_FLAG('a')) != 0;
        positive count = (positive)program_argument_count();

        work.setting = work.minimum || work.maximum ||
                       (work.reset && !pid_text);

        if (!pid_text)
                work.verbose = false;

        if (!system && !pid_text && !work.setting)
                return ul_usage("uclampset",
                                "[options] --pid PID | command [argument ...] | --system");

        if (system)
        {
                if (pid_text || all || taking.first < count)
                        return ul_bad_usage("uclampset", "bad usage");

                string_address paths[] = {
                    "/proc/sys/kernel/sched_util_clamp_min",
                    "/proc/sys/kernel/sched_util_clamp_max",
                };
                p32 values_out[2];

                for (positive at = 0; at < 2; at++)
                {
                        p8 text[32];
                        if ((at == 0 ? work.minimum : work.maximum) &&
                            work.setting)
                        {
                                p32 value = at == 0 ? work.min : work.max;
                                positive length = positive_into_string(
                                    text, value == (p32)-1
                                              ? (at == 0 ? 0 : 1024)
                                              : value);
                                bipolar handle = system_call_4(
                                    syscall(openat), AT_FDCWD,
                                    (positive)paths[at], FILE_WRITE, 0);
                                if (handle < 0 ||
                                    system_call_3(syscall(write), handle,
                                                  (positive)text, length) < 0)
                                {
                                        if (handle >= 0)
                                                system_call_1(syscall(close), handle);
                                        return ul_bad_usage("uclampset",
                                                            "cannot set system clamp");
                                }
                                system_call_1(syscall(close), handle);
                        }

                        positive parsed;
                        positive used;
                        bipolar length = file_slurp(paths[at], text,
                                                    sizeof(text));
                        parsed = string_digits_max(text, 1024, address_of used);
                        if (length <= 0 || !used)
                                values_out[at] = at ? 1024 : 0;
                        else
                                values_out[at] = (p32)parsed;
                }

                string_format(log, "System util_clamp: min: %p max: %p\n",
                              (positive)values_out[0], (positive)values_out[1]);
                log_flush();
                return 0;
        }

        b32 pid = 0;
        if (pid_text)
        {
                if (taking.first < count ||
                    !ul_pid(pid_text, "uclampset", "PID", address_of pid))
                        return 1;
        }
        else if (taking.first >= count || all)
                return ul_bad_usage("uclampset", "bad usage");

        if (ul_tasks(pid, all, ul_uclamp_one, address_of work))
        {
                log_flush();
                return 1;
        }
        log_flush();
        return pid_text ? 0 : ul_exec(taking.first, "uclampset");
}

// flock -----------------------------------------------------------
#define UL_F_OFD_SETLK 37
#define UL_F_OFD_SETLKW 38
#define UL_F_RDLCK 0
#define UL_F_WRLCK 1
#define UL_F_UNLCK 2

typedef struct
{
        p16 type;
        p16 whence;
        b64 start;
        b64 length;
        b32 pid;
        b32 padding;
} ul_flock_range;

static bool ul_duration(string_address text, positive address_to nanoseconds)
{
        string_address at = text;
        positive made = 0;
        positive fraction = 0;
        positive dropped = 0;
        bool point = false;
        bool any = false;

        while (byte_is_space(string_get(at)))
                at++;
        if (string_is(at, '+'))
                at++;

        while (byte_is_digit(string_get(at)) ||
               (!point && string_is(at, '.')))
        {
                if (string_is(at, '.'))
                {
                        point = true;
                        at++;
                        continue;
                }

                positive digit = string_get(at++) - '0';
                any = true;
                if (point)
                        fraction++;
                if (made <= (positive_max - digit) / 10)
                        made = made * 10 + digit;
                else
                        dropped++;
        }
        if (!any)
                return false;

        bipolar exponent = 0;
        if (string_is(at, 'e') || string_is(at, 'E'))
        {
                bool negative = false;
                positive magnitude = 0;

                at++;
                if (string_is(at, '+') || string_is(at, '-'))
                        negative = string_get(at++) == '-';
                if (!byte_is_digit(string_get(at)))
                        return false;
                while (byte_is_digit(string_get(at)))
                {
                        if (magnitude < 1000000)
                                magnitude = magnitude * 10 +
                                            string_get(at) - '0';
                        at++;
                }
                exponent = negative ? -(bipolar)magnitude
                                    : (bipolar)magnitude;
        }
        if (string_get(at))
                return false;

        bipolar scale = 9 + exponent - (bipolar)fraction +
                        (bipolar)dropped;
        if (!made)
        {
                address_to nanoseconds = 0;
                return true;
        }
        while (scale > 0)
        {
                if (made > positive_max / 10)
                        return false;
                made *= 10;
                scale--;
        }
        while (scale < 0 && made)
        {
                made /= 10;
                scale++;
        }
        address_to nanoseconds = made;
        return true;
}

static positive ul_now_ns()
{
        timespec now = {0, 0};

        if (system_call_2(syscall(clock_gettime), UL_CLOCK_MONOTONIC,
                          (positive)address_of now) < 0)
                return 0;
        return (positive)now.tv_sec * 1000000000 + (positive)now.tv_nsec;
}

static bipolar ul_flock_try(b32 handle, p8 kind, bool nonblocking,
                            bool fcntl, positive start, positive length)
{
        if (!fcntl)
        {
                positive operation = kind == 's' ? UL_LOCK_SHARED
                                     : kind == 'u' ? UL_LOCK_UNLOCK
                                                   : UL_LOCK_EXCLUSIVE;
                if (nonblocking)
                        operation |= UL_LOCK_NONBLOCK;
                return system_call_2(syscall(flock), (positive)handle,
                                     operation);
        }

        ul_flock_range range = {
            .type = kind == 's' ? UL_F_RDLCK
                    : kind == 'u' ? UL_F_UNLCK
                                  : UL_F_WRLCK,
            .whence = 0,
            .start = (b64)start,
            .length = (b64)length,
        };
        return system_call_3(syscall(fcntl), (positive)handle,
                             nonblocking ? UL_F_OFD_SETLK : UL_F_OFD_SETLKW,
                             (positive)address_of range);
}

static fn ul_flock_alarm(b32 number)
{
        (void)number;
}

static b32 ul_flock_poll(b32 handle, p8 kind, positive timeout, bool fcntl,
                         positive start, positive length, b32 conflict)
{
        positive began = ul_now_ns();

        for (;;)
        {
                bipolar answer = ul_flock_try(handle, kind, true, fcntl,
                                              start, length);
                if (answer >= 0)
                        return 0;
                if (answer != -UL_ERROR_AGAIN && answer != -ERROR_ACCESS)
                {
                        string_format(file_fail, "flock: cannot lock: %s\n",
                                      file_reason(answer));
                        return 1;
                }
                positive now = ul_now_ns();
                positive elapsed = now >= began ? now - began : timeout;
                if (elapsed >= timeout)
                        return conflict;

                positive left = timeout - elapsed;
                positive nap = left < 10000000 ? left : 10000000;
                timespec span = {nap / 1000000000, nap % 1000000000};
                system_call_2(syscall(nanosleep), (positive)address_of span, 0);
        }
}

static b32 ul_flock_acquire(b32 handle, p8 kind, bool nonblocking,
                            bool timed, positive timeout, bool fcntl,
                            positive start, positive length,
                            b32 conflict)
{
        if (nonblocking || (timed && !timeout))
        {
                bipolar answer = ul_flock_try(handle, kind, true, fcntl,
                                              start, length);
                if (answer >= 0)
                        return 0;
                if (answer == -UL_ERROR_AGAIN || answer == -ERROR_ACCESS)
                        return conflict;
                string_format(file_fail, "flock: cannot lock: %s\n",
                              file_reason(answer));
                return answer == -ERROR_BAD_DESCRIPTOR ? 65 : 1;
        }
        if (!timed)
        {
                bipolar answer = ul_flock_try(handle, kind, false, fcntl,
                                              start, length);
                if (answer >= 0)
                        return 0;
                string_format(file_fail, "flock: cannot lock: %s\n",
                              file_reason(answer));
                return answer == -ERROR_BAD_DESCRIPTOR ? 65 : 1;
        }

        signal_interval prior;
        if (system_call_2(syscall(getitimer), SIGNAL_TIMER_REAL,
                          (positive)address_of prior) < 0 ||
            prior.first_seconds || prior.first_microseconds)
                return ul_flock_poll(handle, kind, timeout, fcntl, start,
                                     length, conflict);

        signal_action wanted;
        signal_action had;
        memory_zero(address_of wanted, sizeof wanted);
        wanted.handler = ul_flock_alarm;
        if (signal_action_change(SIGALRM, address_of wanted, address_of had) < 0)
                return ul_flock_poll(handle, kind, timeout, fcntl, start,
                                     length, conflict);

        signal_interval timer = {0, 0, (bipolar)(timeout / 1000000000),
                                 (bipolar)((timeout % 1000000000 + 999) / 1000)};
        if (timer.first_microseconds == 1000000)
        {
                timer.first_seconds++;
                timer.first_microseconds = 0;
        }
        bipolar answer = system_call_3(syscall(setitimer), SIGNAL_TIMER_REAL,
                                       (positive)address_of timer, 0);
        if (answer >= 0)
                answer = ul_flock_try(handle, kind, false, fcntl,
                                      start, length);

        signal_interval stopped = {0, 0, 0, 0};
        system_call_3(syscall(setitimer), SIGNAL_TIMER_REAL,
                      (positive)address_of stopped, 0);
        signal_action_change(SIGALRM, address_of had, null);

        if (answer >= 0)
                return 0;
        if (answer == UL_ERROR_INTERRUPTED)
                return conflict;
        string_format(file_fail, "flock: cannot lock: %s\n",
                      file_reason(answer));
        return answer == -ERROR_BAD_DESCRIPTOR ? 65 : 1;
}

static b32 ul_flock_exec(string_address address_to words)
{
        b32 answer = ul_exec_words(words, "flock");

        return answer == 126 || answer == 127 ? 69 : answer;
}

static p8 ul_flock_kind;
static const file_supersede ul_flock_supersedes[] = {
    {(string_address)"sxu", address_of ul_flock_kind}, {null, null},
};

static const file_long ul_flock_longs[] = {
    {(string_address)"shared", 's'}, {(string_address)"exclusive", 'x'},
    {(string_address)"unlock", 'u'}, {(string_address)"nb", 'n'},
    {(string_address)"nonblocking", 'n'}, {(string_address)"timeout", 'w'},
    {(string_address)"wait", 'w'},
    {(string_address)"conflict-exit-code", 'E'},
    {(string_address)"close", 'o'}, {(string_address)"command", 'c'},
    {(string_address)"no-fork", 'F'}, {(string_address)"fcntl", 'L'},
    {(string_address)"start", 'S'}, {(string_address)"length", 'N'},
    {(string_address)"verbose", 'v'}, {(string_address)"help", 'h'},
    {(string_address)"version", 'V'}, {null, 0},
};

static b32 util_linux_flock()
{
        file_taking taking = {
            .program = (string_address)"flock",
            .allowed = (string_address)"sxunwEocFVh",
            .valued = (string_address)"wEcSN",
            .longs = ul_flock_longs,
            .supersedes = ul_flock_supersedes,
        };
        b32 answer;

        ul_flock_kind = 0;
        if (!file_take(address_of taking))
                return 64;
        if (ul_meta(address_of taking,
                    "[options] file|directory command [argument ...] | descriptor",
                    address_of answer))
                return answer;

        bool no_fork = (taking.flags & FILE_FLAG('F')) != 0;
        bool close_child = (taking.flags & FILE_FLAG('o')) != 0;
        if (no_fork && close_child)
        {
                ul_bad_usage("flock",
                             "the --no-fork and --close options are incompatible");
                return 64;
        }

        positive count = (positive)program_argument_count();
        if (taking.first >= count)
                return 64;

        b32 conflict = 1;
        positive parsed;
        if (file_option_value(address_of taking, 'E'))
        {
                if (!ul_unsigned(file_option_value(address_of taking, 'E'), 255,
                                 address_of parsed))
                        return 64;
                conflict = (b32)parsed;
        }

        bool timed = file_option_value(address_of taking, 'w') != null;
        positive timeout = 0;
        if (timed && !ul_duration(file_option_value(address_of taking, 'w'),
                                  address_of timeout))
        {
                ul_bad_usage("flock", "invalid timeout");
                return 64;
        }

        bool fcntl = (taking.flags & FILE_FLAG('L')) ||
                     file_option_value(address_of taking, 'S') ||
                     file_option_value(address_of taking, 'N');
        positive start = 0;
        positive length = 0;
        if ((file_option_value(address_of taking, 'S') &&
             !ul_size(file_option_value(address_of taking, 'S'), address_of start)) ||
            (file_option_value(address_of taking, 'N') &&
             !ul_size(file_option_value(address_of taking, 'N'), address_of length)))
        {
                ul_bad_usage("flock", "invalid lock range");
                return 64;
        }

        string_address target = program_argument((b32)taking.first);
        string_address command_text = file_option_value(address_of taking, 'c');
        bool command_option = command_text != null;

        /* GNU getopt accepts the documented `flock file -c command` order.
           The shared scanner deliberately stops at the first operand, so
           consume this one post-operand spelling here instead of teaching
           every file applet to permute options. */
        if (!command_option && taking.first + 1 < count &&
            string_equals(program_argument((b32)taking.first + 1), "-c"))
        {
                if (taking.first + 3 != count)
                        return 64;
                command_text = program_argument((b32)taking.first + 2);
                command_option = true;
        }
        else if (command_option && taking.first + 1 != count)
                return 64;
        bool descriptor = false;
        bipolar descriptor_number = 0;
        if (!command_option && taking.first + 1 == count)
        {
                if (!ul_signed(target, b32_min, b32_max,
                               address_of descriptor_number))
                        return 64;
                descriptor = true;
        }
        b32 handle;

        if (descriptor)
                handle = (b32)descriptor_number;
        else
        {
                handle = (b32)system_call_4(syscall(openat), AT_FDCWD,
                                             (positive)target,
                                             FILE_READ_WRITE | FILE_CREATE,
                                             0666);
                if (handle < 0 &&
                    (!fcntl || ul_flock_kind == 's' ||
                     ul_flock_kind == 'u'))
                        handle = (b32)system_call_4(syscall(openat), AT_FDCWD,
                                                     (positive)target,
                                                     FILE_READ, 0);
                if (handle < 0)
                {
                        string_format(file_fail, "flock: cannot open %s: %s\n",
                                      target, file_reason(handle));
                        return handle == -ERROR_IS_DIRECTORY ? 65 : 66;
                }
        }

        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        positive began = verbose ? ul_now_ns() : 0;
        answer = ul_flock_acquire(handle, ul_flock_kind ? ul_flock_kind : 'x',
                                  (taking.flags & FILE_FLAG('n')) != 0,
                                  timed, timeout, fcntl, start, length,
                                  conflict);
        if (answer && verbose && timed && answer == conflict)
                string_format(file_fail,
                              "flock: timeout while waiting to get lock\n");
        if (!answer && verbose)
        {
                positive elapsed = ul_now_ns() - began;
                p8 fraction_text[32];
                positive_into_string(fraction_text,
                                     (elapsed % 1000000000) / 1000);
                string_format(log, "flock: getting lock took %p.",
                              elapsed / 1000000000);
                string_to_field(log, fraction_text, 6, '0', false);
                string_format(log, " seconds\n");
        }
        if (answer || descriptor)
        {
                if (!descriptor)
                        system_call_1(syscall(close), handle);
                return answer;
        }

        string_address command_words[] = {
            (string_address)"/bin/sh", (string_address)"-c",
            command_text, null,
        };
        string_address address_to words = command_option
            ? command_words : program_argument_list() + taking.first + 1;

        if ((!words[0]) || (!command_option && taking.first + 1 >= count))
        {
                system_call_1(syscall(close), handle);
                return 64;
        }
        if (verbose)
                string_format(log, "flock: executing %s\n", words[0]);

        if (no_fork)
        {
                answer = ul_flock_exec(words);
                system_call_1(syscall(close), handle);
                return answer;
        }

        log_flush();
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
        if (child == 0)
        {
                if (close_child)
                        system_call_1(syscall(close), handle);
                system_call_1(syscall(exit), ul_flock_exec(words));
        }
        if (child < 0)
        {
                system_call_1(syscall(close), handle);
                return 1;
        }

        if (!close_child)
                system_call_1(syscall(close), handle);

        positive status = 0;
        answer = system_wait4_retry(child, address_of status, 0, null) < 0
                   ? 1 : wait_status_code(status);
        if (close_child)
                system_call_1(syscall(close), handle);
        return answer;
}

// setarch ---------------------------------------------------------
#define UL_PER_MASK 0xff
#define UL_UNAME26 0x0020000
#define UL_ADDR_NO_RANDOMIZE 0x0040000
#define UL_FDPIC_FUNCPTRS 0x0080000
#define UL_MMAP_PAGE_ZERO 0x0100000
#define UL_ADDR_COMPAT_LAYOUT 0x0200000
#define UL_READ_IMPLIES_EXEC 0x0400000
#define UL_ADDR_LIMIT_32BIT 0x0800000
#define UL_SHORT_INODE 0x1000000
#define UL_WHOLE_SECONDS 0x2000000
#define UL_STICKY_TIMEOUTS 0x4000000
#define UL_ADDR_LIMIT_3GB 0x8000000

typedef struct { p32 value; string_address name; } ul_named_number;
typedef struct { string_address name; p32 personality; } ul_arch;

static const ul_named_number ul_personalities[] = {
    {0x0000, (string_address)"PER_LINUX"},
    {UL_ADDR_LIMIT_32BIT, (string_address)"PER_LINUX_32BIT"},
    {UL_FDPIC_FUNCPTRS, (string_address)"PER_LINUX_FDPIC"},
    {0x0006, (string_address)"PER_BSD"},
    {0x0006 | UL_STICKY_TIMEOUTS, (string_address)"PER_SUNOS"},
    {0x0008, (string_address)"PER_LINUX32"},
    {0x0008 | UL_ADDR_LIMIT_3GB, (string_address)"PER_LINUX32_3GB"},
    {0x000c, (string_address)"PER_RISCOS"},
    {0x000f, (string_address)"PER_OSF4"},
    {0x0010, (string_address)"PER_HPUX"},
    {0, null},
};

static const ul_named_number ul_personality_flags[] = {
    {UL_UNAME26, (string_address)"UNAME26"},
    {UL_ADDR_NO_RANDOMIZE, (string_address)"ADDR_NO_RANDOMIZE"},
    {UL_FDPIC_FUNCPTRS, (string_address)"FDPIC_FUNCPTRS"},
    {UL_MMAP_PAGE_ZERO, (string_address)"MMAP_PAGE_ZERO"},
    {UL_ADDR_COMPAT_LAYOUT, (string_address)"ADDR_COMPAT_LAYOUT"},
    {UL_READ_IMPLIES_EXEC, (string_address)"READ_IMPLIES_EXEC"},
    {UL_ADDR_LIMIT_32BIT, (string_address)"ADDR_LIMIT_32BIT"},
    {UL_SHORT_INODE, (string_address)"SHORT_INODE"},
    {UL_WHOLE_SECONDS, (string_address)"WHOLE_SECONDS"},
    {UL_STICKY_TIMEOUTS, (string_address)"STICKY_TIMEOUTS"},
    {UL_ADDR_LIMIT_3GB, (string_address)"ADDR_LIMIT_3GB"},
    {0, null},
};

static const ul_arch ul_arches[] = {
    {(string_address)"uname26", UL_UNAME26},
    {(string_address)"linux32", 0x0008},
    {(string_address)"linux64", 0x0000},
#if X64
    {(string_address)"i386", 0x0008}, {(string_address)"i486", 0x0008},
    {(string_address)"i586", 0x0008}, {(string_address)"i686", 0x0008},
    {(string_address)"athlon", 0x0008}, {(string_address)"x86_64", 0x0000},
#elif ARM64
    {(string_address)"armv7l", 0x0008}, {(string_address)"armv8l", 0x0008},
    {(string_address)"armh", 0x0008}, {(string_address)"arm", 0x0008},
    {(string_address)"arm64", 0x0000}, {(string_address)"aarch64", 0x0000},
#elif RISCV64 || RISCV32
    {(string_address)"riscv32", 0x0008}, {(string_address)"rv32", 0x0008},
    {(string_address)"riscv64", 0x0000}, {(string_address)"rv64", 0x0000},
#endif
    {null, 0},
};

static fn ul_hex_field(p32 value, positive width)
{
        positive_to_base_field(log, value, 16, width, -1, (positive)1 << 28);
}

static fn ul_personality_say(p32 personality)
{
        for (positive i = 0; ul_personalities[i].name; i++)
                if (personality == ul_personalities[i].value)
                {
                        string_format(log, "%s\n", ul_personalities[i].name);
                        return;
                }

        p32 options = personality & ~UL_PER_MASK;
        p32 base = personality & UL_PER_MASK;
        bool known = false;

        for (positive i = 0; ul_personalities[i].name; i++)
                if (base == (ul_personalities[i].value & UL_PER_MASK))
                {
                        log(ul_personalities[i].name, 0);
                        known = true;
                        break;
                }
        if (!known)
        {
                log("0x", 2);
                ul_hex_field(base, 2);
        }
        if (options)
        {
                log(" (", 2);
                for (positive i = 0; ul_personality_flags[i].name; i++)
                        if (options & ul_personality_flags[i].value)
                        {
                                log(ul_personality_flags[i].name, 0);
                                options &= ~ul_personality_flags[i].value;
                                if (options)
                                        log(" ", 1);
                        }
                if (options)
                {
                        log("0x", 2);
                        ul_hex_field(options, 8);
                }
                log(")", 1);
        }
        log("\n", 1);
}

static bool ul_personality_number(string_address text, p32 address_to out)
{
        string_address at = text;
        positive value;

        while (byte_is_space(string_get(at)))
                at++;
        bool negative = string_is(at, '-');
        if (negative || string_is(at, '+'))
                at++;
        if (string_is(at, '0') && byte_to_lower(string_get(at + 1)) == 'x')
                at += 2;
        if (!ul_size_number(address_of at, 16, address_of value) ||
            string_get(at) ||
            (!negative && value > 0x7fffffff) ||
            (negative && value > ((positive)1 << 63)))
                return false;
        address_to out = (p32)(negative ? (positive)0 - value : value);
        return true;
}

static b32 ul_setarch_show(string_address value, b32 pid)
{
        p32 personality;

        if (pid)
        {
                p8 path[64] = "/proc/";
                p8 text[32];
                positive at = 6;
                at += positive_into_string(path + at, (positive)(p32)pid);
                memory_copy_apart_end(path + at, "/personality", 12);
                bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                                (positive)path,
                                                FILE_READ | O_CLOEXEC, 0);
                bipolar got = handle < 0 ? handle
                    : system_call_3(syscall(read), handle, (positive)text,
                                    sizeof(text) - 1);
                if (handle >= 0)
                        system_call_1(syscall(close), handle);
                if (got <= 0)
                {
                        string_format(file_fail,
                          "setarch: Can not get the personality for process(%b): %s\n",
                          (bipolar)pid, file_reason(got));
                        return 1;
                }
                text[got] = 0;
                while (got && byte_is_space(text[got - 1]))
                        text[--got] = 0;
                value = text;
        }

        if (value && !string_equals(value, "current"))
        {
                if (!ul_personality_number(value, address_of personality))
                        return ul_bad_usage("setarch", "could not parse personality");
        }
        else
        {
                bipolar got = system_call_1(syscall(personality), (positive)(p32)-1);
                if (got < 0)
                {
                        string_format(file_fail,
                          "setarch: Can not get current kernel personality: %s\n",
                          file_reason(got));
                        return 1;
                }
                personality = (p32)got;
        }
        ul_personality_say(personality);
        log_flush();
        return 0;
}

static const p8 ul_setarch_flag_index[128] = {
    ['R'] = 2, ['F'] = 3, ['Z'] = 4, ['L'] = 5, ['X'] = 6,
    ['B'] = 7, ['I'] = 8, ['S'] = 9, ['T'] = 10, ['3'] = 11,
};

static bool ul_setarch_flag(p8 letter, p32 address_to bit,
                            string_address address_to name)
{
        positive which = letter < 128 ? ul_setarch_flag_index[letter] : 0;
        if (!which)
                return false;
        address_to bit = ul_personality_flags[which - 1].value;
        address_to name = ul_personality_flags[which - 1].name;
        return true;
}

static const file_long ul_setarch_longs[] = {
    {"help", 'h'}, {"version", 'V'}, {"verbose", 'v'},
    {"addr-no-randomize", 'R'}, {"fdpic-funcptrs", 'F'},
    {"mmap-page-zero", 'Z'}, {"addr-compat-layout", 'L'},
    {"read-implies-exec", 'X'}, {"32bit", 'B'}, {"short-inode", 'I'},
    {"whole-seconds", 'S'}, {"sticky-timeouts", 'T'}, {"3gb", '3'},
    {"4gb", '4'}, {"uname-2.6", 'u'}, {"list", 'l'}, {"show", 's'},
    {"pid", 'p'}, {null, 0},
};

static p32 ul_setarch_options;
static bool ul_setarch_verbose;
static bool ul_setarch_list;
static bool ul_setarch_do_show;

static bool ul_setarch_take(p8 letter, string_address value)
{
        if (letter == 'v') ul_setarch_verbose = true;
        else if (letter == 'l') ul_setarch_list = true;
        else if (letter == 's') ul_setarch_do_show = true;
        else if (letter == 'u') {
                ul_setarch_options |= UL_UNAME26;
                if (ul_setarch_verbose) string_format(log, "Switching on UNAME26.\n");
        }
        else
        {
                p32 bit;
                string_address name;
                if (!ul_setarch_flag(letter, address_of bit, address_of name))
                        return true;
                ul_setarch_options |= bit;
                if (ul_setarch_verbose)
                        string_format(log, "Switching on %s.\n", name);
        }
        return true;
}

static b32 util_linux_setarch()
{
        positive count = (positive)program_argument_count(), first = 1;
        string_address arch = null;
        b32 pid = 0;
        b32 answer;
        file_taking taking = {
            .program = "setarch", .allowed = "hVv3BFILRSTXZp",
            .valued = "p", .optional = "s", .longs = ul_setarch_longs,
            .seen = ul_setarch_take,
        };

        if (first < count && !string_is(program_argument((b32)first), '-'))
                arch = program_argument((b32)first++);
        ul_setarch_options = 0;
        ul_setarch_verbose = ul_setarch_list = ul_setarch_do_show = false;
        if (!file_take_from(address_of taking, first)) return 1;
        if (ul_meta(address_of taking,
                    "[<arch>] [options] [<program> [argument ...]]",
                    address_of answer)) return answer;
        if (ul_setarch_list) {
                for (positive i = 0; ul_arches[i].name; i++)
                        string_format(log, "%s\n", ul_arches[i].name);
                log_flush();
                return 0;
        }
        if (taking.flags & FILE_FLAG('p')) {
                if (!ul_pid(file_option_value(address_of taking, 'p'),
                            "setarch", "PID", address_of pid) || !pid) return 1;
        }
        if (ul_setarch_do_show)
                return ul_setarch_show(file_option_value(address_of taking, 's'), pid);
        if (pid) return ul_bad_usage("setarch", "use -p/--pid option with --show option");
        if (!arch && !ul_setarch_options) return ul_bad_usage("setarch", "no architecture argument or personality flags specified");

        p32 personality = ul_setarch_options;
        if (arch)
        {
                positive i;
                for (i = 0; ul_arches[i].name; i++)
                        if (string_equals(arch, ul_arches[i].name)) { personality |= ul_arches[i].personality; break; }
                if (!ul_arches[i].name) { string_format(file_fail, "setarch: %s: Unrecognized architecture\n", arch); return 1; }
        }

        bipolar changed = system_call_1(syscall(personality), personality);
        if (changed < 0) { string_format(file_fail, "setarch: failed to set personality to %s: %s\n", arch ? arch : (string_address)"(null)", file_reason(changed)); return 1; }

        if (taking.first < count)
        {
                if (ul_setarch_verbose) string_format(log, "Execute command `%s'.\n", program_argument((b32)taking.first));
                log_flush();
                return ul_exec(taking.first, "setarch");
        }

        string_address shell_words[2] = {(string_address)"-sh", null};
        if (ul_setarch_verbose) string_format(log, "Execute command `/bin/sh'.\n");
        log_flush();
        changed = system_call_3(syscall(execve), (positive)"/bin/sh",
                                (positive)shell_words,
                                (positive)file_environment_all());
        string_format(file_fail, "setarch: /bin/sh: %s\n", file_reason(changed));
        return changed == -ERROR_NO_ENTRY ? 127 : 126;
}

// waitpid ---------------------------------------------------------
typedef struct
{
        b32 descriptor;
        b16 events;
        b16 returned;
} ul_wait_pid;

static ul_wait_pid address_to ul_wait_pids;
static positive ul_wait_room;

static fn ul_wait_close(positive count)
{
        for (positive i = 0; i < count; i++)
                system_call_1(syscall(close),
                              (positive)ul_wait_pids[i].descriptor);
}

static bool ul_wait_operand(string_address text, b32 address_to pid,
                            p64 address_to inode)
{
        string_address colon = string_first_of(text, ':');
        string_address at = text;
        positive value;

        while (byte_is_space(string_get(at))) at++;
        if (string_is(at, '+')) at++;
        else if (string_is(at, '-')) return false;
        if (!ul_size_number(address_of at, 10, address_of value) || !value ||
            value > b32_max || (colon ? at != colon : string_get(at)))
                return false;
        address_to pid = (b32)value;
        address_to inode = 0;

        if (colon)
        {
                at = colon + 1;
                positive got;
                if (!ul_size_number(address_of at, 10, address_of got) ||
                    string_get(at) || !got)
                        return false;
                address_to inode = got;
        }
        return true;
}

static const file_long ul_waitpid_longs[] = {
    {(string_address)"verbose", 'v'}, {(string_address)"timeout", 't'},
    {(string_address)"exited", 'e'}, {(string_address)"count", 'c'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static b32 util_linux_waitpid()
{
        file_taking taking = {
            .program = (string_address)"waitpid",
            .allowed = (string_address)"vetcVh",
            .valued = (string_address)"tc",
            .longs = ul_waitpid_longs,
            .operand = file_operand,
        };
        b32 answer;

        file_operand_count = 0;
        file_operand_failed = false;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] PID[:inode]...",
                    address_of answer))
                return answer;
        if (file_operand_failed)
                return ul_bad_usage("waitpid", "not enough memory");
        if (!file_operand_count)
                return ul_bad_usage("waitpid", "no PIDs specified");
        if ((taking.flags & FILE_FLAG('e')) &&
            (taking.flags & FILE_FLAG('c')))
                return ul_bad_usage("waitpid",
                                    "options --exited and --count are mutually exclusive");

        positive wanted = file_operand_count;
        positive count = wanted;
        if (taking.flags & FILE_FLAG('c'))
        {
                if (!ul_unsigned(file_option_value(address_of taking, 'c'),
                                 positive_max, address_of count) || !count)
                        return ul_bad_usage("waitpid", "invalid count");
                if (count > wanted)
                        return ul_bad_usage("waitpid",
                                            "count exceeds number of PIDs");
        }

        positive timeout = 0;
        if ((taking.flags & FILE_FLAG('t')) &&
            !ul_duration(file_option_value(address_of taking, 't'),
                         address_of timeout))
                return ul_bad_usage("waitpid", "invalid timeout");

        if (!shell_room((address_any address_to)address_of ul_wait_pids,
                        address_of ul_wait_room, wanted,
                        sizeof(ul_wait_pids[0])) ||
            !shell_room((address_any address_to)address_of file_id_scratch,
                        address_of file_id_scratch_room, wanted,
                        sizeof(file_id_scratch[0])))
                return ul_bad_usage("waitpid", "not enough memory");

        positive active = 0;
        bool allow_exited = (taking.flags & FILE_FLAG('e')) != 0;
        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;

        for (positive i = 0; i < wanted; i++)
        {
                b32 pid;
                p64 inode;
                string_address operand =
                    program_argument(file_operand_list[i]);

                if (!ul_wait_operand(operand, address_of pid,
                                     address_of inode))
                {
                        ul_wait_close(active);
                        string_format(file_fail,
                                      "waitpid: failed to parse PID argument '%s'\n",
                                      operand);
                        return 1;
                }

                bipolar descriptor =
                    system_call_2(syscall(pidfd_open), (positive)(p32)pid, 0);
                if (descriptor < 0)
                {
                        if (allow_exited && descriptor == -3)
                        {
                                if (verbose)
                                        string_format(file_fail,
                                                      "waitpid: PID %b has exited, skipping\n",
                                                      (bipolar)pid);
                                continue;
                        }
                        string_format(file_fail,
                                      "waitpid: could not open PID %b: %s\n",
                                      (bipolar)pid, file_reason(descriptor));
                        ul_wait_close(active);
                        return 1;
                }

                if (inode)
                {
                        file_facts facts;
                        if (!file_look(descriptor, "", AT_EMPTY_PATH,
                                       address_of facts) || facts.inode != inode)
                        {
                                system_call_1(syscall(close), descriptor);
                                if (verbose)
                                        string_format(file_fail,
                                                      "waitpid: pidfd inode %p not found for PID %b\n",
                                                      (positive)inode,
                                                      (bipolar)pid);
                                if (allow_exited)
                                        continue;
                                ul_wait_close(active);
                                return ul_bad_usage("waitpid",
                                                    "could not open PID");
                        }
                }

                ul_wait_pids[active] = (ul_wait_pid){(b32)descriptor, 1, 0};
                file_id_scratch[active++] = pid;
        }

        if (count > active)
                count = active;
        if (!count)
                return 0;

        positive deadline = 0;
        if (timeout)
        {
                positive now = ul_now_ns();
                deadline = timeout > positive_max - now
                    ? positive_max : now + timeout;
        }
        while (count)
        {
                timespec span;
                timespec address_to limit = null;
                if (deadline)
                {
                        positive now = ul_now_ns();
                        if (now >= deadline)
                        {
                                if (verbose)
                                {
                                        log("Timeout expired\n", 16);
                                        log_flush();
                                }
                                ul_wait_close(active);
                                return 3;
                        }
                        positive left = deadline - now;
                        span.tv_sec = left / 1000000000;
                        span.tv_nsec = left % 1000000000;
                        limit = address_of span;
                }

                bipolar ready = system_call_5(syscall(ppoll),
                                               (positive)ul_wait_pids, active,
                                               (positive)limit, 0, 8);
                if (ready == 0)
                {
                        if (verbose)
                        {
                                log("Timeout expired\n", 16);
                                log_flush();
                        }
                        ul_wait_close(active);
                        return 3;
                }
                if (ready < 0)
                {
                        if (ready == -4)
                                continue;
                        string_format(file_fail,
                                      "waitpid: failure during wait: %s\n",
                                      file_reason(ready));
                        ul_wait_close(active);
                        return 1;
                }

                for (positive i = 0; i < active && count; i++)
                {
                        if (!ul_wait_pids[i].returned)
                                continue;
                        if (verbose)
                                string_format(log, "PID %b finished\n",
                                              (bipolar)file_id_scratch[i]);
                        system_call_1(syscall(close),
                                      (positive)ul_wait_pids[i].descriptor);
                        active--;
                        ul_wait_pids[i] = ul_wait_pids[active];
                        file_id_scratch[i] = file_id_scratch[active];
                        i--;
                        count--;
                }
        }

        ul_wait_close(active);
        log_flush();
        return 0;
}

// setpriv ---------------------------------------------------------
#define UL_PR_SET_PDEATHSIG 1
#define UL_PR_GET_PDEATHSIG 2
#define UL_PR_GET_SECUREBITS 27
#define UL_PR_SET_NO_NEW_PRIVS 38
#define UL_PR_GET_NO_NEW_PRIVS 39
#define UL_PR_SET_PTRACER 0x59616d61
#define UL_SECBIT_NOROOT 1
#define UL_SECBIT_NOROOT_LOCKED 2
#define UL_SECBIT_NO_SETUID_FIXUP 4
#define UL_SECBIT_NO_SETUID_FIXUP_LOCKED 8
#define UL_SECBIT_KEEP_CAPS 16
#define UL_SECBIT_KEEP_CAPS_LOCKED 32

static const string_address ul_cap_names[] = {
    "chown", "dac_override", "dac_read_search", "fowner", "fsetid",
    "kill", "setgid", "setuid", "setpcap", "linux_immutable",
    "net_bind_service", "net_broadcast", "net_admin", "net_raw",
    "ipc_lock", "ipc_owner", "sys_module", "sys_rawio", "sys_chroot",
    "sys_ptrace", "sys_pacct", "sys_admin", "sys_boot", "sys_nice",
    "sys_resource", "sys_time", "sys_tty_config", "mknod", "lease",
    "audit_write", "audit_control", "setfcap", "mac_override",
    "mac_admin", "syslog", "wake_alarm", "block_suspend", "audit_read",
    "perfmon", "bpf", "checkpoint_restore",
};

typedef struct
{
        bool ruid_set, euid_set, rgid_set, egid_set;
        p32 ruid, euid, rgid, egid;
        p8 groups;
        string_address group_list;
        bool death_set, ptracer_set;
        b32 death;
        bipolar ptracer;
} ul_setpriv;

static positive ul_setpriv_seen;
static positive ul_setpriv_total;
static positive ul_setpriv_dumps;
static p8 ul_setpriv_group_option;

static bipolar ul_prctl(positive option, positive one, positive two)
{
        return system_call_5(syscall(prctl), option, one, two, 0, 0);
}

static bool ul_word_case(string_address text, positive length,
                         string_address word)
{
        if (string_length(word) != length)
                return false;
        for (positive i = 0; i < length; i++)
                if (byte_to_lower(string_get(text + i)) !=
                    byte_to_lower(string_get(word + i)))
                        return false;
        return true;
}

static bipolar ul_signal_number(string_address text)
{
        p8 name[16];
        positive length = string_length(text);
        if (length >= sizeof(name))
                return -1;
        for (positive i = 0; i < length; i++)
                name[i] = byte_to_upper(string_get(text + i));
        name[length] = 0;
        return kill_number(name);
}

static bool ul_setpriv_take(p8 letter, string_address value)
{
        positive bit = (positive)1 << file_letter_bit(letter);
        ul_setpriv_total++;
        if (letter == 'd')
        {
                ul_setpriv_dumps++;
                return true;
        }
        if (letter == 'h' || letter == 'V')
                return true;
        if ((ul_setpriv_seen & bit) ||
            ((letter == 'c' || letter == 'k' || letter == 'I' || letter == 's') &&
             ul_setpriv_group_option))
        {
                string_format(file_fail, "setpriv: duplicate or mutually exclusive option\n");
                return false;
        }
        ul_setpriv_seen |= bit;
        if (letter == 'c' || letter == 'k' || letter == 'I' || letter == 's')
                ul_setpriv_group_option = letter;
        return true;
}

static bool ul_setpriv_id(string_address text, bool group, p32 address_to id)
{
        positive value;
        if (ul_unsigned(text, p32_max, address_of value))
        {
                address_to id = (p32)value;
                return true;
        }
        bipolar named = group ? file_group_id(text) : file_user_id(text);
        if (named < 0)
                return false;
        address_to id = (p32)named;
        return true;
}

static b32 ul_cap_max = -1;

static COLD b32 ul_cap_last()
{
        if (ul_cap_max >= 0)
                return ul_cap_max;
        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                             (positive)"/proc/sys/kernel/cap_last_cap",
                             FILE_READ | O_CLOEXEC, 0);
        p8 text[24];
        bipolar got = handle < 0 ? handle
            : system_call_3(syscall(read), handle, (positive)text,
                            sizeof(text) - 1);
        if (handle >= 0)
                system_call_1(syscall(close), handle);
        if (got > 0)
        {
                positive taken, value = string_digits_max(text, (positive)got,
                                                          address_of taken);
                if (taken && value < 64)
                        return ul_cap_max = (b32)value;
        }
        return ul_cap_max = 40;
}

static COLD fn ul_caps_say(p64 mask)
{
        bool any = false;
        for (b32 cap = 0; cap <= ul_cap_last(); cap++)
        {
                if (mask & ((p64)1 << cap))
                {
                        if (any) log(",", 1);
                        if ((positive)cap < sizeof(ul_cap_names) / sizeof(ul_cap_names[0]))
                                log(ul_cap_names[cap], 0);
                        else
                                string_format(log, "cap_%b", (bipolar)cap);
                        any = true;
                }
        }
        if (!any) log("[none]", 6);
        log("\n", 1);
}

static COLD bool ul_cap_status(p64 sets[5])
{
        static const string_address names[] = {
            "CapEff:\t", "CapPrm:\t", "CapInh:\t", "CapAmb:\t", "CapBnd:\t",
        };
        p8 text[4096];
        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                       (positive)"/proc/self/status",
                                       FILE_READ | O_CLOEXEC, 0);
        bipolar got = handle < 0 ? handle
            : system_call_3(syscall(read), handle, (positive)text,
                            sizeof(text) - 1);
        if (handle >= 0) system_call_1(syscall(close), handle);
        if (got <= 0) return false;
        text[got] = 0;
        for (positive i = 0; i < 5; i++)
        {
                string_address found = string_search(text, names[i]);
                if (!found) return false;
                found += string_length(names[i]);
                positive value;
                if (!ul_size_number(address_of found, 16, address_of value))
                        return false;
                sets[i] = value;
        }
        return true;
}

static COLD fn ul_setpriv_secure_say(b32 bits)
{
        static const ul_named_number names[] = {
            {UL_SECBIT_NOROOT, "noroot"},
            {UL_SECBIT_NOROOT_LOCKED, "noroot_locked"},
            {UL_SECBIT_NO_SETUID_FIXUP, "no_setuid_fixup"},
            {UL_SECBIT_NO_SETUID_FIXUP_LOCKED, "no_setuid_fixup_locked"},
            {UL_SECBIT_KEEP_CAPS_LOCKED, "keep_caps_locked"}, {0, null},
        };
        bits &= ~UL_SECBIT_KEEP_CAPS;
        bool any = false;
        for (positive i = 0; names[i].name; i++)
                if (bits & names[i].value)
                {
                        if (any) log(",", 1);
                        log(names[i].name, 0);
                        bits &= ~names[i].value;
                        any = true;
                }
        if (bits)
        {
                if (any) log(",", 1);
                log("0x", 2);
                ul_hex_field((p32)bits, 1);
                any = true;
        }
        if (!any) log("[none]", 6);
        log("\n", 1);
}

/* Keep the /proc status block off every ordinary setpriv invocation's stack. */
static COLD __attribute__((noinline)) b32 ul_setpriv_dump()
{
        p32 uid[3], gid[3];
        if (system_call_3(syscall(getresuid), (positive)uid,
                          (positive)(uid + 1), (positive)(uid + 2)) < 0 ||
            system_call_3(syscall(getresgid), (positive)gid,
                          (positive)(gid + 1), (positive)(gid + 2)) < 0)
                return ul_bad_usage("setpriv", "cannot read process IDs");
        string_format(log, "uid: %p\neuid: %p\n", (positive)uid[0],
                      (positive)uid[1]);
        if (ul_setpriv_dumps >= 3)
                string_format(log, "suid: %p\n", (positive)uid[2]);
        string_format(log, "gid: %p\negid: %p\n", (positive)gid[0],
                      (positive)gid[1]);
        if (ul_setpriv_dumps >= 3)
                string_format(log, "sgid: %p\n", (positive)gid[2]);

        bipolar groups = system_call_2(syscall(getgroups), 0, 0);
        if (groups < 0 ||
            !shell_room((address_any address_to)address_of file_id_scratch,
                        address_of file_id_scratch_room, (positive)groups,
                        sizeof(file_id_scratch[0])) ||
            (groups && system_call_2(syscall(getgroups), (positive)groups,
                                     (positive)file_id_scratch) < 0))
                return ul_bad_usage("setpriv", "getgroups failed");
        log("Supplementary groups: ", 22);
        if (!groups) log("[none]", 6);
        for (bipolar i = 0; i < groups; i++)
                string_format(log, i ? ",%p" : "%p",
                              (positive)file_id_scratch[i]);
        log("\n", 1);

        bipolar nnp = ul_prctl(UL_PR_GET_NO_NEW_PRIVS, 0, 0);
        string_format(log, "no_new_privs: %b\n", nnp);
        p64 caps[5];
        if (!ul_cap_status(caps)) return ul_bad_usage("setpriv", "cannot read capability state");
        if (ul_setpriv_dumps >= 2)
        {
                log("Effective capabilities: ", 24); ul_caps_say(caps[0]);
                log("Permitted capabilities: ", 24); ul_caps_say(caps[1]);
        }
        log("Inheritable capabilities: ", 26); ul_caps_say(caps[2]);
        log("Ambient capabilities: ", 22); ul_caps_say(caps[3]);
        log("Capability bounding set: ", 25); ul_caps_say(caps[4]);
        log("Securebits: ", 12);
        ul_setpriv_secure_say((b32)ul_prctl(UL_PR_GET_SECUREBITS, 0, 0));
        b32 death = 0;
        if (ul_prctl(UL_PR_GET_PDEATHSIG, (positive)address_of death, 0) < 0)
                return ul_bad_usage("setpriv", "failed to get parent death signal");
        log("Parent death signal: ", 21);
        if (death > 0) {
                p8 name[16];
                kill_name((positive)(p32)death, name);
                string_format(log, "%s\n", name);
        }
        else
                log("[none]\n", 7);
        log_flush();
        return 0;
}

static const file_long ul_setpriv_longs[] = {
    {"dump", 'd'}, {"nnp", 'n'}, {"no-new-privs", 'n'},
    {"ambient-caps", 'a'}, {"inh-caps", 'i'}, {"bounding-set", 'b'},
    {"ruid", 'r'}, {"euid", 'u'}, {"rgid", 'g'}, {"egid", 'G'},
    {"reuid", 'U'}, {"regid", 'R'}, {"clear-groups", 'c'},
    {"keep-groups", 'k'}, {"init-groups", 'I'}, {"groups", 's'},
    {"list-caps", 'l'}, {"securebits", 'S'}, {"pdeathsig", 'p'},
    {"ptracer", 'P'}, {"selinux-label", 'x'}, {"apparmor-profile", 'A'},
    {"landlock-access", 'L'}, {"landlock-rule", 'D'},
    {"seccomp-filter", 'f'}, {"reset-env", 'e'},
    {"help", 'h'}, {"version", 'V'}, {null, 0},
};

static b32 util_linux_setpriv()
{
        file_taking taking = {
            .program = "setpriv", .allowed = "dhV",
            .valued = "aibrugGURsSpPxALDfe", .longs = ul_setpriv_longs,
            .seen = ul_setpriv_take,
        };
        ul_setpriv set = {0};
        b32 answer;
        ul_setpriv_seen = ul_setpriv_total = ul_setpriv_dumps = 0;
        ul_setpriv_group_option = 0;
        if (!file_take(address_of taking)) return 1;
        if (ul_meta(address_of taking, "[options] program [argument ...]", address_of answer)) return answer;

        if (ul_setpriv_dumps)
        {
                if (ul_setpriv_total != ul_setpriv_dumps || taking.first < (positive)program_argument_count())
                        return ul_bad_usage("setpriv", "--dump is incompatible with all other options");
                return ul_setpriv_dump();
        }
        if (taking.flags & FILE_FLAG('l'))
        {
                if (ul_setpriv_total != 1 || taking.first < (positive)program_argument_count())
                        return ul_bad_usage("setpriv", "--list-caps must be specified alone");
                for (b32 i = 0; i <= ul_cap_last(); i++)
                        if ((positive)i < sizeof(ul_cap_names) / sizeof(ul_cap_names[0]))
                                string_format(log, "%s\n", ul_cap_names[i]);
                log_flush();
                return 0;
        }

        /* Identity/group transitions and the process prctls below are the
           deliberate denominator. Capability mutation, initgroups, LSMs,
           Landlock, seccomp and environment rebuilding are recognized and
           rejected; silently ignoring any of them would be a privilege bug. */
        if (taking.flags & (FILE_FLAG('a') | FILE_FLAG('i') | FILE_FLAG('b') |
                            FILE_FLAG('S') | FILE_FLAG('I') | FILE_FLAG('x') |
                            FILE_FLAG('A') | FILE_FLAG('L') | FILE_FLAG('D') |
                            FILE_FLAG('f') | FILE_FLAG('e')))
                return ul_bad_usage("setpriv", "requested policy is not supported by this build");
        if (taking.first >= (positive)program_argument_count())
                return ul_bad_usage("setpriv", "No program specified");

#define UL_ID(letter, field, is_group) do { if (taking.flags & FILE_FLAG(letter)) { if (!ul_setpriv_id(file_option_value(address_of taking, letter), is_group, address_of set.field)) return ul_bad_usage("setpriv", "failed to parse identity"); set.field##_set = true; } } while (0)
        UL_ID('r', ruid, false); UL_ID('u', euid, false);
        UL_ID('g', rgid, true); UL_ID('G', egid, true);
        if (taking.flags & FILE_FLAG('U')) { if (!ul_setpriv_id(file_option_value(address_of taking, 'U'), false, address_of set.ruid)) return ul_bad_usage("setpriv", "failed to parse reuid"); set.euid = set.ruid; set.ruid_set = set.euid_set = true; }
        if (taking.flags & FILE_FLAG('R')) { if (!ul_setpriv_id(file_option_value(address_of taking, 'R'), true, address_of set.rgid)) return ul_bad_usage("setpriv", "failed to parse regid"); set.egid = set.rgid; set.rgid_set = set.egid_set = true; }
#undef UL_ID
        set.groups = ul_setpriv_group_option;
        set.group_list = file_option_value(address_of taking, 's');
        positive group_count = 0;

        if ((set.rgid_set || set.egid_set) && !set.groups)
                return ul_bad_usage("setpriv", "--[re]gid requires a supplementary group option");
        if (set.groups == 's')
        {
                group_count = 1;
                for (string_address p = set.group_list; string_get(p); p++)
                        group_count += string_is(p, ',');
                if (!shell_room(
                        (address_any address_to)address_of file_id_scratch,
                        address_of file_id_scratch_room, group_count,
                        sizeof(file_id_scratch[0])))
                        return 127;
                string_address p = set.group_list;
                for (positive i = 0; i < group_count; i++)
                {
                        string_address comma = string_first_of(p, ',');
                        positive length = comma ? (positive)(comma - p)
                                                : string_length(p);
                        if (!length || length >= FILE_NAME_MAX)
                                return ul_bad_usage(
                                    "setpriv", "invalid supplementary group id");
                        p8 name[FILE_NAME_MAX];
                        memory_copy_apart(name, p, length);
                        name[length] = 0;
                        if (!ul_setpriv_id(name, true,
                                           file_id_scratch + i))
                                return ul_bad_usage(
                                    "setpriv", "invalid supplementary group id");
                        p = comma ? comma + 1 : p + length;
                }
        }
        if (taking.flags & FILE_FLAG('p'))
        {
                string_address value = file_option_value(address_of taking, 'p');
                set.death_set = true;
                if (string_equals(value, "clear")) set.death = 0;
                else if (string_equals(value, "keep")) {
                        if (ul_prctl(UL_PR_GET_PDEATHSIG, (positive)address_of set.death, 0) < 0) return 127;
                } else if ((set.death = (b32)ul_signal_number(value)) <= 0) return ul_bad_usage("setpriv", "unknown signal");
        }
        if (taking.flags & FILE_FLAG('P'))
        {
                string_address value = file_option_value(address_of taking, 'P');
                set.ptracer_set = true;
                if (string_equals(value, "any")) set.ptracer = -1;
                else if (string_equals(value, "none")) set.ptracer = 0;
                else { b32 pid; if (!ul_pid(value, "setpriv", "PID", address_of pid) || !pid) return 1; set.ptracer = pid; }
        }

        if (set.groups == 'c' &&
            system_call_2(syscall(setgroups), 0, 0) < 0)
                return 127;
        if (set.groups == 's' &&
            system_call_2(syscall(setgroups), group_count,
                          (positive)file_id_scratch) < 0)
                return 127;

        if (set.rgid_set || set.egid_set)
        {
                p32 ids[3];
                if (system_call_3(syscall(getresgid), (positive)ids,
                                  (positive)(ids + 1),
                                  (positive)(ids + 2)) < 0)
                        return 127;
                if (set.rgid_set) ids[0] = set.rgid;
                if (set.egid_set) ids[1] = set.egid;
                if (system_call_3(syscall(setresgid), ids[0], ids[1],
                                  ids[2]) < 0)
                        return 127;
        }
        if (set.ruid_set || set.euid_set)
        {
                p32 ids[3];
                if (system_call_3(syscall(getresuid), (positive)ids,
                                  (positive)(ids + 1),
                                  (positive)(ids + 2)) < 0)
                        return 127;
                if (set.ruid_set) ids[0] = set.ruid;
                if (set.euid_set) ids[1] = set.euid;
                if (system_call_3(syscall(setresuid), ids[0], ids[1],
                                  ids[2]) < 0)
                        return 127;
        }

        if ((taking.flags & FILE_FLAG('n')) &&
            ul_prctl(UL_PR_SET_NO_NEW_PRIVS, 1, 0) < 0)
                return 1;
        if (set.death_set && ul_prctl(UL_PR_SET_PDEATHSIG, (positive)(p32)set.death, 0) < 0) return 127;
        if (set.ptracer_set && ul_prctl(UL_PR_SET_PTRACER, (positive)set.ptracer, 0) < 0) return 127;
        return ul_exec(taking.first, "setpriv");
}


// namespaces ------------------------------------------------------

typedef struct
{
        string_address name;
        p8 option_bit;
        p32 flag;
} ul_namespace;

enum
{
        UL_NS_USER,
        UL_NS_CGROUP,
        UL_NS_IPC,
        UL_NS_UTS,
        UL_NS_NET,
        UL_NS_PID,
        UL_NS_TIME,
        UL_NS_MOUNT,
        UL_NS_COUNT,
};

static const ul_namespace ul_namespaces[] = {
    {(string_address)"user", 46, CLONE_NEWUSER},
    {(string_address)"cgroup", 28, CLONE_NEWCGROUP},
    {(string_address)"ipc", 8, CLONE_NEWIPC},
    {(string_address)"uts", 20, CLONE_NEWUTS},
    {(string_address)"net", 13, CLONE_NEWNET},
    {(string_address)"pid", 15, CLONE_NEWPID},
    {(string_address)"time", 45, CLONE_NEWTIME},
    {(string_address)"mnt", 12, CLONE_NEWNS},
};

static fn ul_proc_path(p8 address_to path, b32 pid,
                       string_address directory, string_address name)
{
        positive at = 6;

        memory_copy_apart(path, "/proc/", at);
        at += positive_into_string(path + at, (positive)(p32)pid);
        path[at++] = '/';
        if (directory)
        {
                positive length = string_length(directory);
                memory_copy_apart(path + at, directory, length);
                at += length;
                path[at++] = '/';
        }
        string_copy_end(path + at, name);
}

static bipolar ul_namespace_open(string_address program, bipolar target_handle,
                                 const ul_namespace address_to space,
                                 string_address path)
{
        p8 made[64];
        string_address relative = null;

        if (!path)
        {
                if (target_handle >= 0)
                {
                        string_copy_end(made, "ns/");
                        string_copy_end(made + 3, space->name);
                        relative = made;
                }
                else
                {
                        string_copy_end(made, "/proc/self/ns/");
                        string_copy_end(made + 14, space->name);
                        path = made;
                }
        }

        bipolar handle = system_call_4(syscall(openat),
                                        relative ? target_handle : AT_FDCWD,
                                        (positive)(relative ? relative : path),
                                        FILE_READ | O_CLOEXEC, 0);
        if (handle < 0)
                string_format(file_fail, "%s: cannot open %s: %s\n",
                              program, relative ? relative : path,
                              file_reason(handle));
        return handle;
}

static bool ul_namespace_same(bipolar handle,
                              const ul_namespace address_to space)
{
        file_facts one;
        file_facts two;
        bipolar own = ul_namespace_open("nsenter", -1, space, null);
        bool same = own >= 0 &&
                    file_look(handle, "", AT_EMPTY_PATH, address_of one) &&
                    file_look(own, "", AT_EMPTY_PATH, address_of two) &&
                    file_same_identity(address_of one, address_of two);

        if (own >= 0)
                system_call_1(syscall(close), own);
        return same;
}

static bipolar ul_directory_open_at(string_address program, bipolar base,
                                    string_address path)
{
        bipolar handle = system_call_4(syscall(openat), base,
                                        (positive)path,
                                        FILE_READ | O_DIRECTORY | O_CLOEXEC,
                                        0);
        if (handle < 0)
                string_format(file_fail, "%s: cannot open %s: %s\n",
                              program, path, file_reason(handle));
        return handle;
}

#define ul_directory_open(program, path) \
        ul_directory_open_at(program, AT_FDCWD, path)

static b32 ul_namespace_write(string_address path, address_any bytes,
                              positive length, string_address program)
{
        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                        (positive)path, FILE_WRITE, 0644);
        bipolar wrote = handle < 0 ? handle
            : system_call_3(syscall(write), handle, (positive)bytes, length);
        bipolar error = wrote < 0 ? wrote
            : (positive)wrote == length ? 0 : -ERROR_INVALID;

        if (handle >= 0)
                system_call_1(syscall(close), handle);
        if (error < 0)
                string_format(file_fail, "%s: cannot write %s: %s\n",
                              program, path, file_reason(error));
        return error < 0;
}

static b32 ul_namespace_map(string_address path, positive inside,
                            positive outside, positive count,
                            string_address program)
{
        p8 line[96];
        positive at = positive_into_string(line, inside);

        line[at++] = ' ';
        at += positive_into_string(line + at, outside);
        line[at++] = ' ';
        at += positive_into_string(line + at, count);
        line[at++] = '\n';
        return ul_namespace_write(path, line, at, program);
}

typedef struct
{
        p32 inside;
        p32 outside;
        p32 count;
} ul_id_map;

static bool ul_namespace_range(string_address text,
                               ul_id_map address_to map)
{
        string_address at = text;
        positive inside;
        positive outside;
        positive count;

        if (!ul_size_number(address_of at, 10, address_of inside) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!ul_size_number(address_of at, 10, address_of outside) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!ul_size_number(address_of at, 10, address_of count) ||
            string_get(at) || !count || inside > p32_max ||
            outside > p32_max || count > p32_max ||
            inside + count > p32_max || outside + count > p32_max)
                return false;
        map->inside = (p32)inside;
        map->outside = (p32)outside;
        map->count = (p32)count;
        return true;
}

static bool ul_namespace_id(string_address text, bool group,
                            positive address_to id)
{
        positive parsed;

        if (ul_unsigned(text, (positive)p32_max - 1, address_of parsed))
        {
                address_to id = parsed;
                return true;
        }

        bipolar named = group ? file_group_id(text) : file_user_id(text);
        if (named < 0)
                return false;
        address_to id = (positive)named;
        return true;
}

static b32 ul_namespace_identity(string_address program,
                                 string_address uid_text,
                                 string_address gid_text,
                                 bool uid_root, bool gid_root,
                                 bipolar follow_handle, bool groups_cleared)
{
        positive uid = 0;
        positive gid = 0;
        file_facts facts;
        bool follow_uid = uid_text && string_equals(uid_text, "follow");
        bool follow_gid = gid_text && string_equals(gid_text, "follow");

        if ((follow_uid || follow_gid) &&
            (follow_handle < 0 ||
             !file_look(follow_handle, "", AT_EMPTY_PATH, address_of facts)))
                return ul_bad_usage(program, "cannot follow target identity");

        if (follow_uid) uid = facts.owner;
        if (follow_gid) gid = facts.group;

        if ((uid_text && !follow_uid &&
             !ul_namespace_id(uid_text, false, address_of uid)) ||
            (gid_text && !follow_gid &&
             !ul_namespace_id(gid_text, true, address_of gid)))
                return ul_bad_usage(program, "invalid user or group");

        if (gid_text && !groups_cleared &&
            system_call_2(syscall(setgroups), 0, 0) < 0)
                return ul_bad_usage(program, "setgroups failed");
        if ((gid_text || gid_root) &&
            system_call_3(syscall(setresgid), gid, gid, gid) < 0)
                return ul_bad_usage(program, "setgid failed");
        if ((uid_text || uid_root) &&
            system_call_3(syscall(setresuid), uid, uid, uid) < 0)
                return ul_bad_usage(program, "setuid failed");
        return 0;
}

static fn ul_namespace_sigchld_default()
{
        positive action[4] = {0, 0, 0, 0};

        system_call_4(syscall(rt_sigaction), SIGCHLD,
                      (positive)address_of action, 0, 8);
}

static b32 ul_namespace_wait(bipolar child, bool job_control)
{
        positive status = 0;

        ul_namespace_sigchld_default();
        while (system_wait4_retry(child, address_of status,
                                  job_control ? 2 : 0, null) >= 0)
        {
                if ((status & 0xff) == 0x7f)
                {
                        system_call_2(syscall(kill),
                                      system_call(syscall(getpid)), 19);
                        system_call_2(syscall(kill), child, 18);
                        continue;
                }
                if (status & 0x7f)
                {
                        b32 signal = (b32)(status & 0x7f);
                        positive action[4] = {0, 0, 0, 0};

                        log_flush();
                        system_call_4(syscall(rt_sigaction), signal,
                                      (positive)address_of action, 0, 8);
                        system_call_2(syscall(kill),
                                      system_call(syscall(getpid)), signal);
                        return 128 + signal;
                }
                return (b32)((status >> 8) & 0xff);
        }
        return 1;
}

static b32 ul_exec_shell(string_address program)
{
        p8 stored[FILE_PATH_MAX];
        string_address shell = file_environment("SHELL");

        if (!shell || !string_get(shell))
        {
                positive at = 0;
                positive uid = (positive)system_call(syscall(geteuid));
                p8 account[FILE_NAME_MAX];
                file_account_record record;

                shell = null;
                if (file_account_name(file_password_text(), uid, 2, account,
                                      sizeof(account)))
                while (file_account_next(file_password_text(), address_of at,
                                         6, address_of record))
                {
                        if (!record.has_value || !record.value_length ||
                            string_length(account) != record.name_length ||
                            memory_compare(account, record.name,
                                           record.name_length))
                                continue;
                        positive length = record.value_length < FILE_PATH_MAX - 1
                            ? record.value_length : FILE_PATH_MAX - 1;
                        memory_copy_apart_end(stored, record.value, length);
                        shell = stored;
                        break;
                }
                if (!shell)
                        shell = "/bin/sh";
        }

        string_address words[] = {(string_address)"", null};
        bipolar error = file_exec_path_try_in(shell, words,
                                              file_environment_all(),
                                              file_environment("PATH"));
        string_format(file_fail, "%s: cannot execute %s: %s\n",
                      program, shell, file_reason(error));
        return error == -ERROR_ACCESS ? 126 : 127;
}

typedef struct
{
        bool uid;
        bool gid;
        bool deny_groups;
        bool clear_groups;
        ul_id_map uid_single;
        ul_id_map gid_single;
        ul_id_map address_to uid_ranges;
        ul_id_map address_to gid_ranges;
        positive uid_range_count;
        positive gid_range_count;
} ul_user_mapping;

static ul_id_map address_to ul_unshare_uid_ranges;
static ul_id_map address_to ul_unshare_gid_ranges;
static positive ul_unshare_uid_range_count;
static positive ul_unshare_gid_range_count;

static bool ul_unshare_seen(p8 letter, string_address value)
{
        if (letter != 'd' && letter != 'g')
                return true;

        ul_id_map address_to map = letter == 'd'
            ? ul_unshare_uid_ranges + ul_unshare_uid_range_count++
            : ul_unshare_gid_ranges + ul_unshare_gid_range_count++;

        if (ul_namespace_range(value, map))
                return true;
        file_complain("unshare", "invalid mapping", value);
        return false;
}

static bool ul_namespace_mapper_present(bool group)
{
        string_address path = group ? (string_address)"/usr/bin/newgidmap"
                                    : (string_address)"/usr/bin/newuidmap";
        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                        (positive)path,
                                        FILE_READ | O_CLOEXEC, 0);

        if (handle >= 0)
                system_call_1(syscall(close), handle);
        else
                string_format(file_fail, "unshare: cannot open %s: %s\n",
                              path, file_reason(handle));
        return handle >= 0;
}

static fn ul_namespace_map_arg(string_address address_to words,
                               p8 address_to address_to numbers,
                               positive address_to at,
                               ul_id_map map)
{
        words[(*at)++] = address_to numbers;
        address_to numbers += positive_into_string(address_to numbers,
                                                   map.inside) + 1;
        words[(*at)++] = address_to numbers;
        address_to numbers += positive_into_string(address_to numbers,
                                                   map.outside) + 1;
        words[(*at)++] = address_to numbers;
        address_to numbers += positive_into_string(address_to numbers,
                                                   map.count) + 1;
}

static b32 ul_namespace_run_mapper(b32 target, bool group,
                                   ul_id_map single, bool has_single,
                                   ul_id_map address_to ranges,
                                   positive range_count)
{
        positive maximum = has_single + range_count * 2;
        string_address words[3 * maximum + 3];
        p8 numbers[11 * (3 * maximum + 1)];
        p8 address_to number = numbers;
        positive at = 0;

        words[at++] = group ? (string_address)"/usr/bin/newgidmap"
                            : (string_address)"/usr/bin/newuidmap";
        words[at++] = number;
        number += positive_into_string(number, (positive)(p32)target) + 1;
        if (has_single)
                ul_namespace_map_arg(words, address_of number, address_of at,
                                     single);
        for (positive i = 0; i < range_count; i++)
        {
                ul_id_map map = ranges[i];

                if (!has_single || single.inside < map.inside ||
                    single.inside - map.inside >= map.count)
                {
                        ul_namespace_map_arg(words, address_of number,
                                             address_of at, map);
                        continue;
                }

                positive before = single.inside - map.inside;
                if (before)
                        ul_namespace_map_arg(words, address_of number,
                                             address_of at,
                                             (ul_id_map){map.inside, map.outside,
                                                         before});
                positive after = map.count - before - 1;
                if (after)
                        ul_namespace_map_arg(words, address_of number,
                                             address_of at,
                                             (ul_id_map){single.inside + 1,
                                                         map.outside + before,
                                                         after});
        }
        words[at] = null;

        ul_namespace_sigchld_default();
        log_flush();
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
        if (!child)
        {
                bipolar error = file_exec_path_try_in(words[0], words,
                                                      file_environment_all(),
                                                      null);
                system_call_1(syscall(exit),
                              error == -ERROR_ACCESS ? 126 : 127);
        }
        return child < 0 ? 1 : ul_namespace_wait(child, false);
}

static b32 ul_namespace_mapping(b32 target, bool group,
                                ul_id_map single, bool has_single,
                                ul_id_map address_to ranges,
                                positive range_count)
{
        if (range_count)
                return ul_namespace_run_mapper(target, group, single,
                                               has_single, ranges, range_count);

        p8 path[64];
        ul_proc_path(path, target, null, group ? (string_address)"gid_map"
                                              : (string_address)"uid_map");
        return has_single && ul_namespace_map(path, single.inside,
                                              single.outside, single.count,
                                              "unshare");
}

/* A task inside a new user namespace cannot authorize its own uid_map.  Leave
   one tiny helper in the old namespace while the original process unshares;
   preserving that original PID matters to callers which background unshare. */
static b32 ul_unshare_user(ul_user_mapping address_to map)
{
        b32 ready[2];

        if (system_call_2(syscall(pipe2), (positive)address_of ready,
                          O_CLOEXEC) < 0)
                return ul_bad_usage("unshare", "cannot make mapping channel");

        b32 target = (b32)system_call(syscall(getpid));
        ul_namespace_sigchld_default();
        log_flush();
        bipolar helper = system_call_2(syscall(clone), SIGCHLD, 0);
        if (helper < 0)
        {
                system_call_1(syscall(close), ready[0]);
                system_call_1(syscall(close), ready[1]);
                return ul_bad_usage("unshare", "fork failed");
        }

        p8 byte = 1;
        if (!helper)
        {
                system_call_1(syscall(close), ready[1]);
                bool failed = system_read_retry((positive)ready[0],
                                                address_of byte, 1) != 1;
                p8 path[64];

                if (!failed && map->deny_groups)
                {
                        ul_proc_path(path, target, null, "setgroups");
                        failed = ul_namespace_write(path, "deny", 4, "unshare");
                }
                if (!failed)
                        failed = ul_namespace_mapping(
                            target, false, map->uid_single, map->uid,
                            map->uid_ranges, map->uid_range_count);
                if (!failed)
                        failed = ul_namespace_mapping(
                            target, true, map->gid_single, map->gid,
                            map->gid_ranges, map->gid_range_count);
                system_call_1(syscall(close), ready[0]);
                log_flush();
                system_call_1(syscall(exit), failed);
                return 1;
        }

        system_call_1(syscall(close), ready[0]);
        bool failed = system_call_1(syscall(unshare), CLONE_NEWUSER) < 0;
        if (failed)
                ul_bad_usage("unshare", "unshare failed");
        if (!failed && map->clear_groups)
        {
                failed = system_call_2(syscall(setgroups), 0, 0) < 0;
                if (failed)
                        ul_bad_usage("unshare", "setgroups failed");
        }
        if (!failed)
                failed = system_write_all((positive)ready[1],
                                          address_of byte, 1) != 1;
        system_call_1(syscall(close), ready[1]);
        b32 answer = ul_namespace_wait(helper, false);
        return failed ? 1 : answer;
}

typedef struct
{
        bipolar child;
        b32 notify;
} ul_namespace_persistence;

static bool ul_namespace_persistence_start(
    file_taking address_to taking, ul_namespace_persistence address_to state)
{
        bool any = false;
        for (positive i = 0; i < UL_NS_COUNT; i++)
                any |= taking->value[ul_namespaces[i].option_bit] != null;
        state->child = -1;
        state->notify = -1;
        if (!any)
                return true;

        b32 channel[2];
        if (system_call_2(syscall(pipe2), (positive)address_of channel,
                          O_CLOEXEC) < 0)
                return false;
        ul_namespace_sigchld_default();
        log_flush();
        state->child = system_call_2(syscall(clone), SIGCHLD, 0);
        if (state->child < 0)
        {
                system_call_1(syscall(close), channel[0]);
                system_call_1(syscall(close), channel[1]);
                return false;
        }
        if (!state->child)
        {
                p8 go;
                bool failed;
                b32 target = (b32)system_call(syscall(getppid));

                system_call_1(syscall(close), channel[1]);
                failed = system_read_retry((positive)channel[0],
                                           address_of go, 1) != 1;
                system_call_1(syscall(close), channel[0]);
                for (positive i = 0; !failed && i < UL_NS_COUNT; i++)
                {
                        string_address destination =
                            taking->value[ul_namespaces[i].option_bit];
                        if (!destination)
                                continue;
                        string_address name = i == UL_NS_PID
                            ? (string_address)"pid_for_children"
                            : i == UL_NS_TIME
                                ? (string_address)"time_for_children"
                                : ul_namespaces[i].name;
                        p8 source[64];
                        ul_proc_path(source, target, "ns", name);
                        if (system_call_5(syscall(mount), (positive)source,
                                          (positive)destination, 0,
                                          MS_BIND, 0) < 0)
                        {
                                file_fail("unshare: cannot bind namespace file\n",
                                          0);
                                failed = true;
                        }
                }
                log_flush();
                system_call_1(syscall(exit), failed);
        }
        system_call_1(syscall(close), channel[0]);
        state->notify = channel[1];
        return true;
}

static b32 ul_namespace_persistence_finish(
    ul_namespace_persistence address_to state, bool run)
{
        if (state->child < 0)
                return 0;
        p8 go = 1;
        bool failed = run && system_write_all((positive)state->notify,
                                              address_of go, 1) != 1;
        system_call_1(syscall(close), state->notify);
        b32 answer = ul_namespace_wait(state->child, false);
        return failed || answer;
}

static const file_long ul_unshare_longs[] = {
    {(string_address)"mount", 'm'}, {(string_address)"uts", 'u'},
    {(string_address)"ipc", 'i'}, {(string_address)"net", 'n'},
    {(string_address)"pid", 'p'}, {(string_address)"user", 'U'},
    {(string_address)"cgroup", 'C'}, {(string_address)"time", 'T'},
    {(string_address)"fork", 'f'}, {(string_address)"mount-proc", 'q'},
    {(string_address)"propagation", 'P'}, {(string_address)"root", 'R'},
    {(string_address)"wd", 'w'}, {(string_address)"setuid", 'S'},
    {(string_address)"setgid", 'G'}, {(string_address)"map-user", 'x'},
    {(string_address)"map-group", 'y'},
    {(string_address)"map-root-user", 'r'},
    {(string_address)"map-current-user", 'c'},
    {(string_address)"map-users", 'd'},
    {(string_address)"map-groups", 'g'},
    {(string_address)"setgroups", 's'},
    {(string_address)"monotonic", 'o'},
    {(string_address)"boottime", 'b'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static positive ul_unshare_propagation(string_address text)
{
        if (string_equals(text, "private")) return MS_PRIVATE;
        if (string_equals(text, "shared")) return MS_SHARED;
        if (string_equals(text, "slave")) return MS_SLAVE;
        if (string_equals(text, "unchanged")) return 0;
        return positive_max;
}

static b32 ul_unshare_time(file_taking address_to taking, bool apply)
{
        p8 line[128];
        positive at = 0;
        string_address values[] = {
            file_option_value(taking, 'o'), file_option_value(taking, 'b')};
        string_address names[] = {
            (string_address)"monotonic ", (string_address)"boottime "};

        for (positive which = 0; which < 2; which++)
        {
                if (!values[which])
                        continue;
                bipolar offset;
                if (!nice_adjustment(values[which], address_of offset))
                        return ul_bad_usage("unshare", "invalid time offset");
                positive length = string_length(names[which]);
                memory_copy_apart(line + at, names[which], length);
                at += length;
                at += bipolar_into_string(line + at, offset);
                memory_copy_apart(line + at, " 0\n", 3);
                at += 3;
        }

        return at && apply
            ? ul_namespace_write("/proc/self/timens_offsets", line, at,
                                 "unshare") : 0;
}

static b32 util_linux_unshare()
{
        positive count = (positive)program_argument_count();
        ul_id_map uid_ranges[count];
        ul_id_map gid_ranges[count];
        p8 uid_choice = 0;
        p8 gid_choice = 0;
        const file_supersede choices[] = {
            {(string_address)"rcx", address_of uid_choice},
            {(string_address)"rcy", address_of gid_choice},
            {null, null},
        };
        file_taking taking = {
            .program = (string_address)"unshare",
            .allowed = (string_address)"muinpUCTfrcRwSGVh",
            .valued = (string_address)"PRwSGxydgsob",
            .optional = (string_address)"q",
            .long_optional = (string_address)"muinpUCT",
            .sticky_optional = (string_address)"muinpUCT",
            .longs = ul_unshare_longs,
            .seen = ul_unshare_seen,
            .supersedes = choices,
        };
        b32 answer;

        ul_unshare_uid_ranges = uid_ranges;
        ul_unshare_gid_ranges = gid_ranges;
        ul_unshare_uid_range_count = ul_unshare_gid_range_count = 0;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] [program [argument ...]]",
                    address_of answer))
                return answer;

        positive flags = 0;
        for (positive at = 0; at < UL_NS_COUNT; at++)
                if (taking.flags &
                    ((positive)1 << ul_namespaces[at].option_bit))
                        flags |= ul_namespaces[at].flag;

        string_address map_user = file_option_value(address_of taking, 'x');
        string_address map_group = file_option_value(address_of taking, 'y');
        if (uid_choice || gid_choice || ul_unshare_uid_range_count ||
            ul_unshare_gid_range_count)
                flags |= CLONE_NEWUSER;

        if (taking.flags & FILE_FLAG('q'))
                flags |= CLONE_NEWNS;
        if ((file_option_value(address_of taking, 'o') ||
             file_option_value(address_of taking, 'b')) &&
            !(flags & CLONE_NEWTIME))
                return ul_bad_usage("unshare",
                                    "time offset requires time namespace");
        if (ul_unshare_time(address_of taking, false))
                return 1;
        string_address setting = file_option_value(address_of taking, 's');
        if (setting && !(flags & CLONE_NEWUSER))
                return ul_bad_usage("unshare",
                                    "setgroups requires user namespace");
        positive propagation = file_option_value(address_of taking, 'P')
            ? ul_unshare_propagation(file_option_value(address_of taking, 'P'))
            : MS_PRIVATE;
        if (propagation == positive_max)
                return ul_bad_usage("unshare", "invalid propagation mode");
        string_address set_uid = file_option_value(address_of taking, 'S');
        string_address set_gid = file_option_value(address_of taking, 'G');
        positive checked;
        if ((set_uid && (string_equals(set_uid, "follow") ||
                         !ul_namespace_id(set_uid, false,
                                          address_of checked))) ||
            (set_gid && (string_equals(set_gid, "follow") ||
                         !ul_namespace_id(set_gid, true,
                                          address_of checked))))
                return ul_bad_usage("unshare", "invalid user or group");

        positive other = flags & ~(positive)CLONE_NEWUSER;
        if (flags & CLONE_NEWUSER)
        {
                positive real_uid = (positive)system_call(syscall(geteuid));
                positive real_gid = (positive)system_call(syscall(getegid));
                ul_user_mapping map = {
                    .uid = uid_choice != 0,
                    .gid = gid_choice != 0,
                    .uid_single = {uid_choice == 'r' ? 0 : real_uid,
                                   real_uid, 1},
                    .gid_single = {gid_choice == 'r' ? 0 : real_gid,
                                   real_gid, 1},
                    .uid_ranges = uid_ranges,
                    .gid_ranges = gid_ranges,
                    .uid_range_count = ul_unshare_uid_range_count,
                    .gid_range_count = ul_unshare_gid_range_count,
                    .clear_groups = set_gid != null,
                };

                positive id;
                if (uid_choice == 'x')
                {
                        if (!ul_namespace_id(map_user, false, address_of id))
                                return ul_bad_usage("unshare", "invalid map user");
                        map.uid_single.inside = (p32)id;
                }
                if (gid_choice == 'y')
                {
                        if (!ul_namespace_id(map_group, true, address_of id))
                                return ul_bad_usage("unshare", "invalid map group");
                        map.gid_single.inside = (p32)id;
                }

                if (setting && !string_equals(setting, "deny") &&
                    !string_equals(setting, "allow"))
                        return ul_bad_usage("unshare", "invalid setgroups mode");
                map.deny_groups = setting ? string_equals(setting, "deny")
                    : map.gid || map.gid_range_count;

                if ((map.uid_range_count && !ul_namespace_mapper_present(false)) ||
                    (map.gid_range_count && !ul_namespace_mapper_present(true)))
                        return 1;

                if (map.uid || map.gid || map.uid_range_count ||
                    map.gid_range_count || map.deny_groups ||
                    map.clear_groups)
                {
                        answer = ul_unshare_user(address_of map);
                        if (answer)
                                return answer;
                }
                else if (system_call_1(syscall(unshare), CLONE_NEWUSER) < 0)
                        return ul_bad_usage("unshare", "unshare failed");
        }

        ul_namespace_persistence persistence;
        if (!ul_namespace_persistence_start(address_of taking,
                                            address_of persistence))
                return ul_bad_usage("unshare", "cannot fork persistence helper");
        if (other && system_call_1(syscall(unshare), other) < 0)
        {
                ul_namespace_persistence_finish(address_of persistence, false);
                return ul_bad_usage("unshare", "unshare failed");
        }
        if (ul_unshare_time(address_of taking, true))
        {
                ul_namespace_persistence_finish(address_of persistence, false);
                return 1;
        }

        if ((flags & CLONE_NEWNS) && propagation &&
            system_call_5(syscall(mount), 0, (positive)"/", 0,
                          MS_REC | propagation, 0) < 0)
        {
                ul_namespace_persistence_finish(address_of persistence, false);
                return ul_bad_usage("unshare", "cannot change propagation");
        }
        if (ul_namespace_persistence_finish(address_of persistence, true))
                return 1;

        if (taking.flags & FILE_FLAG('f'))
        {
                positive blocked = ((positive)1 << 1) | ((positive)1 << 14);
                positive old = 0;

                if (system_call_4(syscall(rt_sigprocmask), UL_SIGNAL_BLOCK,
                                  (positive)address_of blocked,
                                  (positive)address_of old, 8) < 0)
                        return ul_bad_usage("unshare", "cannot block signals");
                ul_namespace_sigchld_default();
                log_flush();
                bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
                if (child < 0)
                {
                        system_call_4(syscall(rt_sigprocmask),
                                      UL_SIGNAL_SET_MASK,
                                      (positive)address_of old, 0, 8);
                        return ul_bad_usage("unshare", "fork failed");
                }
                if (child > 0)
                {
                        answer = ul_namespace_wait(child, false);
                        system_call_4(syscall(rt_sigprocmask),
                                      UL_SIGNAL_SET_MASK,
                                      (positive)address_of old, 0, 8);
                        return answer;
                }
                system_call_4(syscall(rt_sigprocmask), UL_SIGNAL_SET_MASK,
                              (positive)address_of old, 0, 8);
        }

        string_address root = file_option_value(address_of taking, 'R');
        string_address wd = file_option_value(address_of taking, 'w');
        if (root && (system_call_1(syscall(chdir), (positive)root) < 0 ||
                     system_call_1(syscall(chroot), (positive)".") < 0 ||
                     system_call_1(syscall(chdir), (positive)"/") < 0))
                return ul_bad_usage("unshare", "cannot change root");
        if (wd && system_call_1(syscall(chdir), (positive)wd) < 0)
                return ul_bad_usage("unshare", "cannot change directory");

        string_address proc = file_option_value(address_of taking, 'q');
        if (taking.flags & FILE_FLAG('q'))
        {
                string_address target = proc ? proc : (string_address)"/proc";
                if (!root && propagation != MS_PRIVATE)
                {
                        bipolar changed = system_call_5(
                            syscall(mount), 0, (positive)target, 0,
                            MS_PRIVATE | MS_REC, 0);
                        if (changed < 0 && (!proc || changed != -ERROR_INVALID))
                                return ul_bad_usage("unshare",
                                                    "cannot privatize proc");
                }
                if (system_call_5(syscall(mount), (positive)"proc",
                                  (positive)target, (positive)"proc",
                                  MS_NOSUID | MS_NODEV | MS_NOEXEC, 0) < 0)
                        return ul_bad_usage("unshare", "cannot mount proc");
        }

        if (ul_namespace_identity("unshare",
                                  set_uid, set_gid,
                                  uid_choice == 'r', gid_choice == 'r', -1,
                                  (flags & CLONE_NEWUSER) != 0 && set_gid))
                return 1;

        return taking.first < count
            ? ul_exec(taking.first, "unshare")
            : ul_exec_shell("unshare");
}

static const file_long ul_nsenter_longs[] = {
    {(string_address)"all", 'a'}, {(string_address)"target", 't'},
    {(string_address)"mount", 'm'}, {(string_address)"uts", 'u'},
    {(string_address)"ipc", 'i'}, {(string_address)"net", 'n'},
    {(string_address)"pid", 'p'}, {(string_address)"user", 'U'},
    {(string_address)"cgroup", 'C'}, {(string_address)"time", 'T'},
    {(string_address)"setuid", 'S'}, {(string_address)"setgid", 'G'},
    {(string_address)"preserve-credentials", 'q'},
    {(string_address)"root", 'r'}, {(string_address)"wd", 'w'},
    {(string_address)"wdns", 'W'}, {(string_address)"no-fork", 'F'},
    {(string_address)"help", 'h'}, {(string_address)"version", 'V'},
    {null, 0},
};

static b32 util_linux_nsenter()
{
        file_taking taking = {
            .program = (string_address)"nsenter",
            .allowed = (string_address)"atmuinpUCTSGrwWFVh",
            .valued = (string_address)"tSG",
            .optional = (string_address)"muinpUCTrwW",
            .sticky_optional = (string_address)"muinpUCTrwW",
            .longs = ul_nsenter_longs,
        };
        b32 answer;
        positive count = (positive)program_argument_count();

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] [program [argument ...]]",
                    address_of answer))
                return answer;

        b32 target = 0;
        bipolar target_handle = -1;
        if (file_option_value(address_of taking, 't') &&
            !ul_pid(file_option_value(address_of taking, 't'), "nsenter",
                    "target PID", address_of target))
                return 1;
        if (target)
        {
                p8 target_path[64];
                ul_proc_path(target_path, target, null, "");
                target_handle = ul_directory_open("nsenter", target_path);
                if (target_handle < 0)
                        return 1;
        }

        bool all = (taking.flags & FILE_FLAG('a')) != 0;
        bipolar handles[UL_NS_COUNT];
        memory_fill(handles, -1, sizeof(handles));
        bipolar root_handle = -1;
        bipolar wd_handle = -1;
        bipolar old_cwd = -1;
        bool entered_user = false;
        bool entered_pid = false;
        positive selected = 0;
        string_address root = file_option_value(address_of taking, 'r');
        string_address wd = file_option_value(address_of taking, 'w');
        string_address wdns = file_option_value(address_of taking, 'W');
        bool target_root = (taking.bare & FILE_FLAG('r')) != 0;
        bool target_wd = (taking.bare & FILE_FLAG('w')) != 0;
        bool caller_wd = (taking.bare & FILE_FLAG('W')) != 0;

        if (caller_wd)
        {
                old_cwd = ul_directory_open("nsenter", ".");
                if (old_cwd < 0)
                        goto nsenter_failed;
        }

        if (target_root)
        {
                if (target_handle < 0)
                        return ul_bad_usage("nsenter", "target PID is required");
                root_handle = ul_directory_open_at("nsenter", target_handle,
                                                   "root");
        }
        else if (root)
        {
                root_handle = ul_directory_open("nsenter", root);
        }
        if (root_handle >= 0)
        {
                if (old_cwd < 0)
                {
                        old_cwd = ul_directory_open("nsenter", ".");
                        if (old_cwd < 0)
                                goto nsenter_failed;
                }
        }
        else if (target_root || root)
                goto nsenter_failed;
        if (target_wd)
        {
                if (target_handle < 0)
                {
                        ul_bad_usage("nsenter", "target PID is required");
                        goto nsenter_failed;
                }
                wd_handle = ul_directory_open_at("nsenter", target_handle,
                                                 "cwd");
        }
        else if (wd)
        {
                wd_handle = ul_directory_open("nsenter", wd);
        }
        if ((target_wd || wd) && wd_handle < 0)
                goto nsenter_failed;

        string_address uid = file_option_value(address_of taking, 'S');
        string_address gid = file_option_value(address_of taking, 'G');
        positive checked;
        if ((uid && !string_equals(uid, "follow") &&
             !ul_namespace_id(uid, false, address_of checked)) ||
            (gid && !string_equals(gid, "follow") &&
             !ul_namespace_id(gid, true, address_of checked)))
        {
                ul_bad_usage("nsenter", "invalid user or group");
                goto nsenter_failed;
        }
        if ((uid && string_equals(uid, "follow")) ||
            (gid && string_equals(gid, "follow")))
        {
                if (target_handle < 0)
                {
                        ul_bad_usage("nsenter", "target PID is required");
                        goto nsenter_failed;
                }
        }
        bool preserve = (taking.flags & FILE_FLAG('q')) != 0;
        bool explicit_user =
            (taking.flags & ((positive)1 << ul_namespaces[UL_NS_USER].option_bit)) != 0;
        bipolar cleared = gid ? system_call_2(syscall(setgroups), 0, 0) : 0;
        bool groups_cleared = gid && (cleared >= 0 || preserve);
        if (cleared < 0 && !preserve)
        {
                ul_bad_usage("nsenter", "setgroups failed");
                goto nsenter_failed;
        }

        for (positive at = 0; at < UL_NS_COUNT; at++)
        {
                if (!all && !(taking.flags &
                              ((positive)1 << ul_namespaces[at].option_bit)))
                        continue;
                string_address path =
                    taking.value[ul_namespaces[at].option_bit];
                if (!path && !target)
                {
                        ul_bad_usage("nsenter", "target PID is required");
                        goto nsenter_failed;
                }
                handles[at] = ul_namespace_open("nsenter", target_handle,
                                                ul_namespaces + at, path);
                if (handles[at] < 0)
                        goto nsenter_failed;
                if (at == UL_NS_USER && all && !explicit_user &&
                    ul_namespace_same(handles[at], ul_namespaces + at))
                {
                        system_call_1(syscall(close), handles[at]);
                        handles[at] = -1;
                        continue;
                }
                selected++;
        }
        if (!selected)
        {
                ul_bad_usage("nsenter", "no namespace specified");
                goto nsenter_failed;
        }
        if (!preserve && handles[UL_NS_USER] >= 0 && !groups_cleared)
        {
                if (system_call_2(syscall(setgroups), 0, 0) < 0)
                {
                        ul_bad_usage("nsenter", "setgroups failed");
                        goto nsenter_failed;
                }
                groups_cleared = true;
        }

        /* Keep the namespaces current credentials can enter, then acquire
           target-user capabilities and retry only those which needed them. */
        for (positive pass = 0; pass < 2; pass++)
        for (positive which = 0; which < UL_NS_COUNT; which++)
        {
                if (handles[which] < 0 || (!pass && which == UL_NS_USER))
                        continue;
                bipolar changed = system_call_2(syscall(setns), handles[which],
                                                 ul_namespaces[which].flag);
                if (changed < 0)
                {
                        if (!pass)
                                continue;
                        ul_bad_usage("nsenter",
                                     "reassociate to namespace failed");
                        goto nsenter_failed;
                }
                system_call_1(syscall(close), handles[which]);
                handles[which] = -1;
                entered_user |= which == UL_NS_USER;
                entered_pid |= which == UL_NS_PID;
        }

        if (root_handle >= 0 &&
            (system_call_1(syscall(fchdir), root_handle) < 0 ||
             system_call_1(syscall(chroot), (positive)".") < 0 ||
             (wd_handle < 0 && (caller_wd || !wdns) &&
              system_call_1(syscall(fchdir), old_cwd) < 0)))
        {
                ul_bad_usage("nsenter", "cannot change root");
                goto nsenter_failed;
        }
        if (wd_handle >= 0 && system_call_1(syscall(fchdir), wd_handle) < 0)
        {
                ul_bad_usage("nsenter", "cannot change directory");
                goto nsenter_failed;
        }
        if (caller_wd && root_handle < 0 &&
            system_call_1(syscall(fchdir), old_cwd) < 0)
        {
                ul_bad_usage("nsenter", "cannot change directory");
                goto nsenter_failed;
        }
        if (!caller_wd && wdns &&
            system_call_1(syscall(chdir), (positive)wdns) < 0)
        {
                ul_bad_usage("nsenter", "cannot change directory");
                goto nsenter_failed;
        }
        if (root_handle >= 0) system_call_1(syscall(close), root_handle);
        if (wd_handle >= 0) system_call_1(syscall(close), wd_handle);
        if (old_cwd >= 0) system_call_1(syscall(close), old_cwd);
        root_handle = wd_handle = old_cwd = -1;

        if ((uid || gid || (!preserve && entered_user)) &&
            ul_namespace_identity("nsenter", uid, gid,
                                  !preserve && entered_user,
                                  !preserve && entered_user, target_handle,
                                  groups_cleared))
                goto nsenter_failed;
        if (target_handle >= 0)
        {
                system_call_1(syscall(close), target_handle);
                target_handle = -1;
        }

        if (entered_pid && !(taking.flags & FILE_FLAG('F')))
        {
                ul_namespace_sigchld_default();
                log_flush();
                bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
                if (child < 0)
                        return ul_bad_usage("nsenter", "fork failed");
                if (child > 0)
                        return ul_namespace_wait(child, true);
        }

        return taking.first < count
            ? ul_exec(taking.first, "nsenter")
            : ul_exec_shell("nsenter");

nsenter_failed:
        if (target_handle >= 0) system_call_1(syscall(close), target_handle);
        if (root_handle >= 0) system_call_1(syscall(close), root_handle);
        if (wd_handle >= 0) system_call_1(syscall(close), wd_handle);
        if (old_cwd >= 0) system_call_1(syscall(close), old_cwd);
        for (positive at = 0; at < UL_NS_COUNT; at++)
                if (handles[at] >= 0)
                        system_call_1(syscall(close), handles[at]);
        return 1;
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
