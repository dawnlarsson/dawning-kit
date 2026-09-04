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

#define UL_FALLOC_KEEP_SIZE 0x01
#define UL_FALLOC_PUNCH_HOLE 0x02
#define UL_FALLOC_COLLAPSE_RANGE 0x08
#define UL_FALLOC_ZERO_RANGE 0x10
#define UL_FALLOC_INSERT_RANGE 0x20
#define UL_FALLOC_WRITE_ZEROES 0x80

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

        if (!string_digits_checked(address_of at, 10, address_of got) ||
            string_get(at) || got > maximum)
                return false;

        address_to value = got;
        return true;
}

/* strtol refuses a digit string that wrapped.  The shell's nice scanner
   wraps silently, which let 18446744073709551616 reach renice as zero. */
static bool ul_signed(string_address text, bipolar minimum, bipolar maximum,
                      bipolar address_to value)
{
        string_address at = text;
        positive magnitude;
        bool negative;

        while (byte_is_space(string_get(at)))
                at++;
        negative = string_is(at, '-');
        if (negative || string_is(at, '+'))
                at++;

        if (!string_digits_checked(address_of at, 10, address_of magnitude) ||
            string_get(at) ||
            magnitude > (positive)bipolar_max + (negative ? 1 : 0))
                return false;

        bipolar got = bipolar_from_magnitude(magnitude, negative);

        if (got < minimum || got > maximum)
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

static bipolar ul_identity(string_address text, positive maximum, bool group)
{
        positive value;

        if (ul_unsigned(text, maximum, address_of value))
                return (bipolar)value;

        return group ? file_group_id(text) : file_user_id(text);
}

/* taskset, chrt and uclampset all give -a the same meaning. */
static b32 ul_tasks(b32 pid, bool all, ul_task_action action,
                    address_any context)
{
        if (!all)
                return action(pid, context);

        p8 path[64];

        system_process_path(path, (p32)pid, null, "task");

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
        positive digit = digit_known(byte, 16);
        return digit < 16 ? (b32)digit : -1;
}

/* Trailing whitespace is noise on an operand and on a procfs line alike. */
static positive ul_trimmed(p8 address_to text, positive length)
{
        while (length && byte_is_space(text[length - 1]))
                length--;
        return length;
}

/* One procfs scalar as a word: the kernel ends the record with a newline
   and no parser here wants it, nor any padding after the value. */
static bipolar ul_slurp_word(string_address path, p8 address_to text,
                             positive room)
{
        bipolar got = file_slurp(path, text, room);

        if (got > 0)
                text[ul_trimmed(text, (positive)got)] = end;
        return got;
}

static bool ul_cpu_mask(string_address text, positive address_to set)
{
        positive length = ul_trimmed(text, string_length(text));
        positive nibble = 0;
        bool any = false;

        memory_fill(set, 0, UL_CPU_WORDS * sizeof(*set));

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

                if (!string_digits_checked(address_of at, 10, address_of first) ||
                    first >= UL_CPU_BITS)
                        return false;

                last = first;
                if (string_is(at, '-'))
                {
                        at++;
                        if (!string_digits_checked(address_of at, 10, address_of last) ||
                            last < first || last >= UL_CPU_BITS)
                                return false;
                }

                if (string_is(at, ':'))
                {
                        at++;
                        if (!string_digits_checked(address_of at, 10,
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

        if (!string_digits_checked(address_of at, base, address_of whole))
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
                    !string_digits_checked(address_of at, 10, address_of fraction))
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

static bipolar ul_path_write(string_address path, address_any bytes,
                             positive length)
{
        bipolar handle = system_open_at_mode(AT_FDCWD,
                                        path, FILE_WRITE, 0644);
        if (handle < 0)
                return handle;

        bipolar wrote = system_write_all((positive)handle, bytes, length);
        system_close(handle);

        return wrote < 0 ? wrote
             : (positive)wrote == length ? 0 : -ERROR_INVALID;
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

#define UL_RESOURCES (array_count(ul_resources))

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
                        if (file_same_word(text, length, ul_limit_headers[at]))
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

#define UL_POLICIES (array_count(ul_policies))

static inline INLINE ul_policy const address_to ul_policy_find(p32 key,
                                                               bool option)
{
        for (positive at = 0; at < UL_POLICIES; at++)
                if (option ? ul_policies[at].option == key
                           : ul_policies[at].value == key)
                        return ul_policies + at;
        return null;
}

static fn ul_chrt_report(b32 pid, string_address state,
                         ul_sched_attr address_to attr)
{
        ul_policy const address_to policy = ul_policy_find(attr->policy, false);

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
            ul_policy_find(ul_chrt_policy ? ul_chrt_policy : 'r', true);
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
        for (positive at = 0; at < array_count(parameters); at++)
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
        p8 path[64];

        system_process_path(path, (p32)pid, null, "comm");
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
                                bipolar handle = system_open_at_mode(
                                    AT_FDCWD, paths[at], FILE_WRITE, 0);
                                if (handle < 0 ||
                                    system_write_once(handle, text, length) < 0)
                                {
                                        if (handle >= 0)
                                                system_close(handle);
                                        return ul_bad_usage("uclampset",
                                                            "cannot set system clamp");
                                }
                                system_close(handle);
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
        positive began = clock_monotonic_nanoseconds();

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
                positive now = clock_monotonic_nanoseconds();
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
                handle = (b32)system_open_at_mode(AT_FDCWD,
                                             target,
                                             FILE_READ_WRITE | FILE_CREATE,
                                             0666);
                if (handle < 0 &&
                    (!fcntl || ul_flock_kind == 's' ||
                     ul_flock_kind == 'u'))
                        handle = (b32)system_open_at(AT_FDCWD,
                                                     target,
                                                     FILE_READ);
                if (handle < 0)
                {
                        string_format(file_fail, "flock: cannot open %s: %s\n",
                                      target, file_reason(handle));
                        return handle == -ERROR_IS_DIRECTORY ? 65 : 66;
                }
        }

        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        positive began = verbose ? clock_monotonic_nanoseconds() : 0;
        answer = ul_flock_acquire(handle, ul_flock_kind ? ul_flock_kind : 'x',
                                  (taking.flags & FILE_FLAG('n')) != 0,
                                  timed, timeout, fcntl, start, length,
                                  conflict);
        if (answer && verbose && timed && answer == conflict)
                string_format(file_fail,
                              "flock: timeout while waiting to get lock\n");
        if (!answer && verbose)
        {
                positive elapsed = clock_monotonic_nanoseconds() - began;
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
                        system_close(handle);
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
                system_close(handle);
                return 64;
        }
        if (verbose)
                string_format(log, "flock: executing %s\n", words[0]);

        if (no_fork)
        {
                answer = ul_flock_exec(words);
                system_close(handle);
                return answer;
        }

        log_flush();
        bipolar child = system_fork();
        if (child == 0)
        {
                if (close_child)
                        system_close(handle);
                system_call_1(syscall(exit), ul_flock_exec(words));
        }
        if (child < 0)
        {
                system_close(handle);
                return 1;
        }

        if (!close_child)
                system_close(handle);

        positive status = 0;
        answer = system_wait4_retry(child, address_of status, 0, null) < 0
                   ? 1 : wait_status_code(status);
        if (close_child)
                system_close(handle);
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
        if (!string_digits_checked(address_of at, 16, address_of value) ||
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
                p8 path[64];
                p8 text[32];

                system_process_path(path, (p32)pid, null,
                                    "personality");
                bipolar got = ul_slurp_word(path, text, sizeof(text));
                if (got <= 0)
                {
                        string_format(file_fail,
                          "setarch: Can not get the personality for process(%b): %s\n",
                          (bipolar)pid, file_reason(got));
                        return 1;
                }
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
        changed = system_execute("/bin/sh", shell_words, file_environment_all());
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
                system_close(ul_wait_pids[i].descriptor);
}

static COLD b32 ul_wait_timed_out(positive active, bool verbose)
{
        if (verbose)
        {
                log("Timeout expired\n", 16);
                log_flush();
        }
        ul_wait_close(active);
        return 3;
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
        if (!string_digits_checked(address_of at, 10, address_of value) || !value ||
            value > b32_max || (colon ? at != colon : string_get(at)))
                return false;
        address_to pid = (b32)value;
        address_to inode = 0;

        if (colon)
        {
                at = colon + 1;
                positive got;
                if (!string_digits_checked(address_of at, 10, address_of got) ||
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

        file_operands_begin();
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

        if (!shell_array_room(ul_wait_pids, ul_wait_room, wanted) ||
            !shell_array_room(file_id_scratch, file_id_scratch_room, wanted))
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
                                system_close(descriptor);
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
                positive now = clock_monotonic_nanoseconds();
                deadline = timeout > positive_max - now
                    ? positive_max : now + timeout;
        }
        while (count)
        {
                timespec span;
                timespec address_to limit = null;
                if (deadline)
                {
                        positive now = clock_monotonic_nanoseconds();
                        if (now >= deadline)
                                return ul_wait_timed_out(active, verbose);
                        positive left = deadline - now;
                        span.tv_sec = left / 1000000000;
                        span.tv_nsec = left % 1000000000;
                        limit = address_of span;
                }

                bipolar ready = system_call_5(syscall(ppoll),
                                               (positive)ul_wait_pids, active,
                                               (positive)limit, 0, 8);
                if (ready == 0)
                        return ul_wait_timed_out(active, verbose);
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
                        system_close(ul_wait_pids[i].descriptor);
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
        bipolar value = ul_identity(text, p32_max, group);

        if (value < 0)
                return false;

        address_to id = (p32)value;
        return true;
}

static b32 ul_cap_max = -1;

static COLD b32 ul_cap_last()
{
        if (ul_cap_max >= 0)
                return ul_cap_max;
        p8 text[24];
        bipolar got = file_slurp("/proc/sys/kernel/cap_last_cap", text,
                                 sizeof(text));
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
                        if ((positive)cap < array_count(ul_cap_names))
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
        bipolar got = file_slurp("/proc/self/status", text, sizeof(text));
        if (got <= 0) return false;
        for (positive i = 0; i < 5; i++)
        {
                string_address found = string_search(text, names[i]);
                if (!found) return false;
                found += string_length(names[i]);
                positive value;
                if (!string_digits_checked(address_of found, 16, address_of value))
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
            !shell_array_room(file_id_scratch, file_id_scratch_room, (positive)groups) ||
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
                        if ((positive)i < array_count(ul_cap_names))
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

        /* Upstream hands the effective id to the saved slot as well,
           setres*id(r, e, e): a saved id left behind is one the program
           could switch back to. */
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
                                  ids[1]) < 0)
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
                                  ids[1]) < 0)
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

        bipolar handle = system_open_at(
                                        relative ? target_handle : AT_FDCWD,
                                        (relative ? relative : path),
                                        FILE_READ | O_CLOEXEC);
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
                system_close(own);
        return same;
}

// lsns ------------------------------------------------------------

enum
{
        UL_LSNS_NS,
        UL_LSNS_TYPE,
        UL_LSNS_PATH,
        UL_LSNS_NPROCS,
        UL_LSNS_PID,
        UL_LSNS_PPID,
        UL_LSNS_COMMAND,
        UL_LSNS_UID,
        UL_LSNS_USER,
        UL_LSNS_COLUMNS,
};

typedef struct
{
        string_address name;
        string_address heading;
        positive width;
        bool number;
        p8 json;
        bool multiline;
} ul_table_column;

#define UL_TABLE_STRING 0
#define UL_TABLE_NUMBER 1
#define UL_TABLE_BOOLEAN 2
#define UL_TABLE_NULL_STRING 3
#define UL_TABLE_NULL_NUMBER 4

static const ul_table_column ul_lsns_columns[] = {
    {(string_address)"ns", (string_address)"NS", 10, true, UL_TABLE_NUMBER},
    {(string_address)"type", (string_address)"TYPE", 0, false, UL_TABLE_STRING},
    {(string_address)"path", (string_address)"PATH", 0, false, UL_TABLE_STRING},
    {(string_address)"nprocs", (string_address)"NPROCS", 5, true, UL_TABLE_NUMBER},
    {(string_address)"pid", (string_address)"PID", 5, true, UL_TABLE_NUMBER},
    {(string_address)"ppid", (string_address)"PPID", 4, true, UL_TABLE_NUMBER},
    {(string_address)"command", (string_address)"COMMAND", 0, false, UL_TABLE_STRING},
    {(string_address)"uid", (string_address)"UID", 3, true, UL_TABLE_NUMBER},
    {(string_address)"user", (string_address)"USER", 0, false, UL_TABLE_STRING},
};

typedef struct
{
        p64 inode;
        struct snapshot_process address_to process;
        string_address command;
        string_address user;
        p32 processes;
        p8 type;
} ul_lsns_entry;

static system_snapshot ul_lsns_snapshot;

static const file_long ul_lsns_longs[] = {
    {(string_address)"json", 'J'},
    {(string_address)"list", 'l'},
    {(string_address)"noheadings", 'n'},
    {(string_address)"output", 'o'},
    {(string_address)"output-all", 'A'},
    {(string_address)"persistent", 'P'},
    {(string_address)"task", 'p'},
    {(string_address)"raw", 'r'},
    {(string_address)"notruncate", 'u'},
    {(string_address)"nowrap", 'W'},
    {(string_address)"type", 't'},
    {(string_address)"tree", 'T'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static PURE bipolar ul_lsns_order(ul_lsns_entry left, ul_lsns_entry right)
{
        if (left.inode != right.inode)
                return left.inode < right.inode ? -1 : 1;
        if (left.type != right.type)
                return left.type < right.type ? -1 : 1;
        if (left.process->pid != right.process->pid)
                return left.process->pid < right.process->pid ? -1 : 1;
        return 0;
}

static b32 ul_lsns_type(string_address name)
{
        for (positive i = 0; i < UL_NS_COUNT; i++)
                if (string_equals(name, ul_namespaces[i].name))
                        return (b32)i;

        return -1;
}

static bool ul_table_column_add(p8 address_to columns, positive address_to count,
                                positive maximum, p8 column)
{
        for (positive i = 0; i < address_to count; i++)
                if (columns[i] == column)
                        return true;

        if (address_to count == maximum)
                return false;

        columns[(address_to count)++] = column;
        return true;
}

static bool ul_table_column_list(
    string_address text, const ul_table_column address_to definitions,
    positive definition_count, const p8 address_to defaults,
    positive default_count, p8 address_to columns, positive address_to count)
{
        bool append = string_is(text, '+');

        address_to count = 0;
        if (append)
        {
                text++;
                for (positive i = 0; i < default_count; i++)
                        columns[(address_to count)++] = defaults[i];
        }

        while (string_get(text))
        {
                string_address comma = string_first_of(text, ',');
                positive length = comma ? (positive)(comma - text)
                                        : string_length(text);
                positive found = definition_count;

                for (positive i = 0; i < definition_count; i++)
                        if (file_same_word(text, length,
                                           definitions[i].name))
                        {
                                found = i;
                                break;
                        }

                if (found == definition_count ||
                    !ul_table_column_add(columns, count, definition_count,
                                         (p8)found))
                        return false;

                text += length;
                if (!string_get(text))
                        break;
                text++;
        }

        return address_to count != 0;
}

static string_address ul_lsns_field(ul_lsns_entry address_to entry,
                                    p8 column, p8 address_to scratch)
{
        struct snapshot_process address_to process = entry->process;

        switch (column)
        {
        case UL_LSNS_NS:
                positive_into_string(scratch, (positive)entry->inode);
                return scratch;
        case UL_LSNS_TYPE:
                return ul_namespaces[entry->type].name;
        case UL_LSNS_PATH:
                system_process_path(scratch, process->pid, "ns",
                                    ul_namespaces[entry->type].name);
                return scratch;
        case UL_LSNS_NPROCS:
                positive_into_string(scratch, entry->processes);
                return scratch;
        case UL_LSNS_PID:
                positive_into_string(scratch, process->pid);
                return scratch;
        case UL_LSNS_PPID:
                positive_into_string(scratch, process->ppid);
                return scratch;
        case UL_LSNS_COMMAND:
                return entry->command;
        case UL_LSNS_UID:
                positive_into_string(scratch, process->uid);
                return scratch;
        default:
                return entry->user;
        }
}

typedef string_address (*ul_table_field)(address_any row, p8 column,
                                         p8 address_to scratch);

static string_address ul_lsns_table_field(address_any row, p8 column,
                                          p8 address_to scratch)
{
        return ul_lsns_field((ul_lsns_entry address_to)row, column, scratch);
}

/* libsmartcols' raw mode protects field separators.  Process arguments may
   also contain literal controls or backslashes, so keep every record on one
   unambiguous physical line instead of escaping spaces alone. */
static fn ul_lsns_raw(string_address text)
{
        static const p8 hex[] = "0123456789abcdef";

        while (string_get(text))
        {
                p8 byte = string_get(text++);

                if (byte > ' ' && byte < 0x7f && byte != '\\')
                        log(text - 1, 1);
                else
                {
                        p8 escaped[4] = {'\\', 'x', hex[byte >> 4],
                                         hex[byte & 15]};
                        log(escaped, sizeof(escaped));
                }
        }
}

/* Normal smartcols output leaves separators readable but still quotes bytes
   that could manufacture a second physical row. */
static PURE positive ul_lsns_safe_length(string_address text)
{
        positive length = 0;

        while (string_get(text))
        {
                p8 byte = string_get(text++);
                length += byte < ' ' || byte == 0x7f ? 4 : 1;
        }
        return length;
}

static PURE positive ul_table_line_count(string_address text,
                                         bool multiline)
{
        positive count = 1;
        if (multiline)
                while (string_get(text))
                        if (string_get(text++) == '\n')
                                count++;
        return count;
}

static string_address ul_table_line(string_address text, positive wanted,
                                    positive address_to length)
{
        while (wanted && string_get(text))
                if (string_get(text++) == '\n')
                        wanted--;
        if (wanted)
        {
                address_to length = 0;
                return (string_address)"";
        }
        string_address stop = text;
        while (string_get(stop) && string_get(stop) != '\n')
                stop++;
        address_to length = (positive)(stop - text);
        return text;
}

static PURE positive ul_lsns_safe_span_length(string_address text,
                                               positive bytes)
{
        positive length = 0;
        for (positive i = 0; i < bytes; i++)
        {
                p8 byte = string_get(text + i);
                length += byte < ' ' || byte == 0x7f ? 4 : 1;
        }
        return length;
}

static PURE positive ul_table_safe_width(string_address text, bool multiline)
{
        if (!multiline)
                return ul_lsns_safe_length(text);
        positive width = 0;
        positive lines = ul_table_line_count(text, true);
        for (positive line = 0; line < lines; line++)
        {
                positive bytes;
                string_address part = ul_table_line(text, line,
                                                     address_of bytes);
                width = max(width, ul_lsns_safe_span_length(part, bytes));
        }
        return width;
}

static fn ul_lsns_safe(string_address text)
{
        static const p8 hex[] = "0123456789abcdef";

        while (string_get(text))
        {
                p8 byte = string_get(text++);

                if (byte >= ' ' && byte != 0x7f)
                        log(text - 1, 1);
                else
                {
                        p8 escaped[4] = {'\\', 'x', hex[byte >> 4],
                                         hex[byte & 15]};
                        log(escaped, sizeof(escaped));
                }
        }
}

static fn ul_lsns_safe_span(string_address text, positive bytes)
{
        static const p8 hex[] = "0123456789abcdef";
        for (positive i = 0; i < bytes; i++)
        {
                p8 byte = string_get(text + i);
                if (byte >= ' ' && byte != 0x7f)
                        log(text + i, 1);
                else
                {
                        p8 escaped[4] = {'\\', 'x', hex[byte >> 4],
                                         hex[byte & 15]};
                        log(escaped, sizeof(escaped));
                }
        }
}

static fn ul_lsns_safe_span_field(string_address text, positive bytes,
                                  positive width, bool left)
{
        positive length = ul_lsns_safe_span_length(text, bytes);
        positive padding = width > length ? width - length : 0;
        if (!left)
                writer_fill(log, padding, ' ');
        ul_lsns_safe_span(text, bytes);
        if (left)
                writer_fill(log, padding, ' ');
}

static fn ul_lsns_safe_field(string_address text, positive width, bool left)
{
        positive length = ul_lsns_safe_length(text);
        positive padding = width > length ? width - length : 0;

        if (!left)
                writer_fill(log, padding, ' ');
        ul_lsns_safe(text);
        if (left)
                writer_fill(log, padding, ' ');
}

static fn ul_lsns_json_string(string_address text)
{
        static const p8 hex[] = "0123456789abcdef";

        log("\"", 1);
        while (string_get(text))
        {
                p8 byte = string_get(text++);

                if (byte == '"' || byte == '\\')
                {
                        p8 escaped[2] = {'\\', byte};
                        log(escaped, sizeof(escaped));
                }
                else if (byte < ' ')
                {
                        p8 escaped[6] = {'\\', 'u', '0', '0',
                                         hex[byte >> 4], hex[byte & 15]};
                        log(escaped, sizeof(escaped));
                }
                else
                        log(text - 1, 1);
        }
        log("\"", 1);
}

static fn ul_table_json(string_address name, address_any rows,
                        positive row_size, positive count,
                        const ul_table_column address_to definitions,
                        p8 address_to columns, positive column_count,
                        ul_table_field field_of)
{
        string_format(log, "{\n   \"%s\": [", name);

        for (positive row = 0; row < count; row++)
        {
                log(row ? ",{\n" : "\n      {\n", row ? 3 : 9);

                for (positive field = 0; field < column_count; field++)
                {
                        p8 column = columns[field];
                        p8 scratch[96];
                        string_address value = field_of(
                            (p8 address_to)rows + row * row_size, column,
                            scratch);

                        if (field)
                                log(",\n", 2);
                        string_format(log, "         \"%s\": ",
                                      definitions[column].name);
                        p8 json = definitions[column].json;

                        if ((json == UL_TABLE_NULL_STRING ||
                             json == UL_TABLE_NULL_NUMBER) &&
                            !string_get(value))
                                log("null", 4);
                        else if (json == UL_TABLE_BOOLEAN)
                        {
                                bool false_value = string_equals(value, "0") ||
                                                   string_equals(value, "no");
                                log(false_value ? "false" : "true",
                                    false_value ? 5 : 4);
                        }
                        else if (json == UL_TABLE_NUMBER ||
                                 json == UL_TABLE_NULL_NUMBER)
                                log(value, string_length(value));
                        else
                                ul_lsns_json_string(value);
                }
                log("\n      }", 8);
        }

        if (!count)
                log("\n", 1);
        log("\n   ]\n}\n", 8);
}

static fn ul_table_out(address_any rows, positive row_size, positive count,
                       const ul_table_column address_to definitions,
                       positive definition_count, p8 address_to columns,
                       positive column_count, bool headings, bool raw,
                       ul_table_field field_of)
{
        positive widths[64] = {0};

        if (!count || definition_count > array_count(widths))
                return;

        for (positive i = 0; i < column_count; i++)
        {
                p8 column = columns[i];
                widths[column] = definitions[column].width;
        }

        if (headings)
                for (positive i = 0; i < column_count; i++)
                {
                        p8 column = columns[i];
                        positive heading = string_length(
                            definitions[column].heading);

                        if (heading > widths[column])
                                widths[column] = heading;
                }

        if (!raw)
                for (positive row = 0; row < count; row++)
                        for (positive field = 0; field < column_count; field++)
                        {
                                p8 column = columns[field];
                                p8 scratch[96];
                                positive length = ul_table_safe_width(
                                    field_of((p8 address_to)rows +
                                                 row * row_size,
                                             column, scratch),
                                    definitions[column].multiline);

                                if (length > widths[column])
                                        widths[column] = length;
                        }

        for (positive row = 0; row < count + (headings ? 1 : 0); row++)
        {
                bool heading = headings && !row;
                address_any entry = (p8 address_to)rows +
                    (heading ? 0 : row - (headings ? 1 : 0)) * row_size;

                positive lines = 1;
                if (!heading && !raw)
                        for (positive field = 0; field < column_count; field++)
                        {
                                p8 column = columns[field];
                                if (!definitions[column].multiline)
                                        continue;
                                p8 scratch[96];
                                positive have = ul_table_line_count(
                                    field_of(entry, column, scratch), true);
                                lines = max(lines, have);
                        }

                for (positive line = 0; line < lines; line++)
                {
                        for (positive field = 0; field < column_count; field++)
                        {
                                p8 column = columns[field];
                                p8 scratch[96];
                                string_address value = heading
                                    ? definitions[column].heading
                                    : field_of(entry, column, scratch);
                                positive bytes = string_length(value);
                                if (!raw && !heading)
                                {
                                        if (definitions[column].multiline)
                                                value = ul_table_line(
                                                    value, line,
                                                    address_of bytes);
                                        else if (line)
                                                bytes = 0;
                                }

                                if (field)
                                        log(" ", 1);
                                if (raw)
                                        ul_lsns_raw(value);
                                else
                                {
                                        bool number =
                                            definitions[column].number;
                                        bool last_text =
                                            field + 1 == column_count &&
                                            !number;
                                        bool empty_last =
                                            field + 1 == column_count &&
                                            !bytes;

                                        ul_lsns_safe_span_field(
                                            value, bytes,
                                            (last_text || empty_last)
                                                ? 0 : widths[column],
                                            !number);
                                }
                        }
                        log("\n", 1);
                }
        }
}

static b32 util_linux_lsns()
{
        file_taking taking = {
            .program = (string_address)"lsns",
            .allowed = (string_address)"JlnoprutPWVh",
            .valued = (string_address)"optT",
            .longs = ul_lsns_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] [namespace]",
                    address_of answer))
                return answer;

        if (taking.flags & FILE_FLAG('T'))
                return ul_bad_usage("lsns", "tree output is not supported");
        if (taking.flags & FILE_FLAG('P'))
                return ul_bad_usage("lsns",
                                    "persistent namespace discovery is not supported");
        if (taking.flags & FILE_FLAG('A'))
                return ul_bad_usage("lsns", "--output-all is not supported");
        if ((taking.flags & FILE_FLAG('J')) &&
            (taking.flags & FILE_FLAG('r')))
                return ul_bad_usage("lsns", "--json and --raw are mutually exclusive");

        positive argument_count = (positive)program_argument_count();
        positive operands = argument_count - taking.first;

        if (operands > 1)
                return ul_bad_usage("lsns", "too many namespace operands");
        if (operands && file_option_value(address_of taking, 'p'))
                return ul_bad_usage("lsns",
                                    "--task and a namespace operand are mutually exclusive");

        b32 type = -1;
        string_address type_name = file_option_value(address_of taking, 't');

        if (type_name && (type = ul_lsns_type(type_name)) < 0)
        {
                string_format(file_fail, "lsns: unknown namespace type: %s\n",
                              type_name);
                return 1;
        }

        b32 task = 0;
        bool task_selected = file_option_value(address_of taking, 'p') != null;
        if (task_selected &&
            !ul_pid(file_option_value(address_of taking, 'p'), "lsns", "PID",
                    address_of task))
                return 1;

        positive wanted_inode = 0;
        bool inode_selected = operands != 0;
        if (inode_selected &&
            !ul_unsigned(program_argument((b32)taking.first), positive_max,
                         address_of wanted_inode))
                return ul_bad_usage("lsns", "invalid namespace inode");

        static const p8 defaults[] = {
            UL_LSNS_NS, UL_LSNS_TYPE, UL_LSNS_NPROCS,
            UL_LSNS_PID, UL_LSNS_USER, UL_LSNS_COMMAND,
        };
        static const p8 inode_defaults[] = {
            UL_LSNS_PID, UL_LSNS_PPID, UL_LSNS_USER, UL_LSNS_COMMAND,
        };
        p8 columns[UL_LSNS_COLUMNS];
        positive column_count = 0;
        string_address output = file_option_value(address_of taking, 'o');

        if (output)
        {
                const p8 address_to initial = inode_selected
                    ? inode_defaults : defaults;
                positive initial_count = inode_selected
                    ? array_count(inode_defaults) : array_count(defaults);

                if (!ul_table_column_list(
                        output, ul_lsns_columns, UL_LSNS_COLUMNS, initial,
                        initial_count, columns, address_of column_count))
                        return ul_bad_usage("lsns", "unknown output column");
        }
        else if (inode_selected)
        {
                columns[column_count++] = UL_LSNS_PID;
                columns[column_count++] = UL_LSNS_PPID;
                columns[column_count++] = UL_LSNS_USER;
                columns[column_count++] = UL_LSNS_COMMAND;
        }
        else
                for (positive i = 0; i < array_count(defaults); i++)
                        columns[column_count++] = defaults[i];

        text_begin("lsns");
        text_arena_used = 0;

        if (!system_snapshot_take(address_of ul_lsns_snapshot,
                                  SPARK_SNAPSHOT_PROCESS, true))
                return text_refuse("/proc", "cannot read", 1);

        positive process_count = ul_lsns_snapshot.header.process_count;
        positive type_count = type < 0 ? UL_NS_COUNT : 1;

        if (process_count > positive_max / type_count ||
            process_count * type_count >
                TEXT_ARENA_BYTES / sizeof(ul_lsns_entry))
                return text_refuse(null, "too many processes", 1);

        positive capacity = process_count * type_count;
        ul_lsns_entry address_to entries =
            (ul_lsns_entry address_to)text_arena_take(
                capacity * sizeof(ul_lsns_entry));

        if (!entries)
                return text_done(1);

        positive used = 0;
        p64 task_inodes[UL_NS_COUNT] = {0};

        for (positive p = 0; p < process_count; p++)
        {
                struct snapshot_process address_to process =
                    ul_lsns_snapshot.processes + p;
                positive first_type = type < 0 ? 0 : (positive)type;
                positive stop_type = type < 0 ? UL_NS_COUNT : first_type + 1;

                for (positive kind = first_type; kind < stop_type; kind++)
                {
                        p8 path[64];
                        file_facts facts;

                        system_process_path(path, process->pid, "ns",
                                            ul_namespaces[kind].name);
                        if (!file_look_at(path, address_of facts))
                                continue;

                        entries[used++] = (ul_lsns_entry){
                            .inode = facts.inode,
                            .process = process,
                            .processes = 1,
                            .type = (p8)kind,
                        };
                        if (task_selected && process->pid == (p32)task)
                                task_inodes[kind] = facts.inode;
                }
        }

        if (used)
        {
                ul_lsns_entry address_to spare =
                    (ul_lsns_entry address_to)text_arena_take(
                        used * sizeof(ul_lsns_entry));

                if (!spare)
                        return text_done(1);
                entries = array_merge_sort(entries, spare, used,
                                           ul_lsns_order);
        }

        positive groups = 0;
        bool inode_found = false;

        for (positive i = 0; i < used;)
        {
                positive next = i + 1;

                while (next < used && entries[next].inode == entries[i].inode &&
                       entries[next].type == entries[i].type)
                        next++;

                bool keep = (!task_selected ||
                             task_inodes[entries[i].type] == entries[i].inode) &&
                            (!inode_selected ||
                             wanted_inode == entries[i].inode);

                if (keep && inode_selected)
                {
                        for (positive member = i; member < next; member++)
                        {
                                entries[groups] = entries[member];
                                entries[groups].processes = (p32)(next - i);
                                entries[groups].command =
                                    ps_arguments(entries[groups].process);
                                entries[groups].user =
                                    ps_name_of(entries[groups].process->uid);

                                if (!entries[groups].command ||
                                    !entries[groups].user)
                                        return text_done(1);
                                groups++;
                        }
                        inode_found = true;
                }
                else if (keep)
                {
                        entries[groups] = entries[i];
                        entries[groups].processes = (p32)(next - i);
                        entries[groups].command =
                            ps_arguments(entries[groups].process);
                        entries[groups].user =
                            ps_name_of(entries[groups].process->uid);

                        if (!entries[groups].command || !entries[groups].user)
                                return text_done(1);

                        groups++;
                        inode_found = true;
                }

                i = next;
        }

        if (inode_selected && !inode_found)
        {
                string_format(file_fail, "lsns: not found namespace: %p\n",
                              wanted_inode);
                return 1;
        }

        if (taking.flags & FILE_FLAG('J'))
                ul_table_json("namespaces", entries, sizeof(*entries), groups,
                              ul_lsns_columns, columns, column_count,
                              ul_lsns_table_field);
        else
                ul_table_out(entries, sizeof(*entries), groups,
                             ul_lsns_columns, UL_LSNS_COLUMNS, columns,
                             column_count,
                             !(taking.flags & FILE_FLAG('n')),
                             (taking.flags & FILE_FLAG('r')) != 0,
                             ul_lsns_table_field);

        log_flush();
        return 0;
}

// lslocks ---------------------------------------------------------

enum
{
        UL_LOCKS_COMMAND,
        UL_LOCKS_PID,
        UL_LOCKS_TYPE,
        UL_LOCKS_SIZE,
        UL_LOCKS_INODE,
        UL_LOCKS_DEVICE,
        UL_LOCKS_MODE,
        UL_LOCKS_MANDATORY,
        UL_LOCKS_START,
        UL_LOCKS_END,
        UL_LOCKS_PATH,
        UL_LOCKS_BLOCKER,
        UL_LOCKS_HOLDERS,
        UL_LOCKS_COLUMNS,
};

static ul_table_column ul_lslocks_columns[] = {
    {(string_address)"command", (string_address)"COMMAND", 0, false, UL_TABLE_STRING},
    {(string_address)"pid", (string_address)"PID", 5, true, UL_TABLE_NUMBER},
    {(string_address)"type", (string_address)"TYPE", 5, true, UL_TABLE_STRING},
    {(string_address)"size", (string_address)"SIZE", 4, true, UL_TABLE_NULL_STRING},
    {(string_address)"inode", (string_address)"INODE", 5, true, UL_TABLE_NUMBER},
    {(string_address)"maj:min", (string_address)"MAJ:MIN", 7, false, UL_TABLE_STRING},
    {(string_address)"mode", (string_address)"MODE", 5, false, UL_TABLE_STRING},
    {(string_address)"m", (string_address)"M", 1, true, UL_TABLE_BOOLEAN},
    {(string_address)"start", (string_address)"START", 5, true, UL_TABLE_NUMBER},
    {(string_address)"end", (string_address)"END", 3, true, UL_TABLE_NUMBER},
    {(string_address)"path", (string_address)"PATH", 0, false, UL_TABLE_NULL_STRING},
    {(string_address)"blocker", (string_address)"BLOCKER", 7, true, UL_TABLE_NULL_NUMBER},
    {(string_address)"holders", (string_address)"HOLDERS", 0, false, UL_TABLE_NULL_STRING},
};

typedef struct
{
        p64 id;
        p64 inode;
        p64 start;
        p64 finish;
        p64 size;
        struct snapshot_process address_to process;
        string_address path;
        b32 pid;
        b32 blocker;
        p32 major;
        p32 minor;
        p8 type;
        p8 mode;
        bool mandatory;
        bool blocked;
        bool size_known;
        bool accessible;
} ul_lslocks_entry;

static system_snapshot ul_lslocks_snapshot;
static bool ul_lslocks_bytes;

static const file_long ul_lslocks_longs[] = {
    {(string_address)"bytes", 'b'},
    {(string_address)"json", 'J'},
    {(string_address)"noinaccessible", 'i'},
    {(string_address)"noheadings", 'n'},
    {(string_address)"output", 'o'},
    {(string_address)"output-all", 'A'},
    {(string_address)"pid", 'p'},
    {(string_address)"filter", 'Q'},
    {(string_address)"raw", 'r'},
    {(string_address)"notruncate", 'u'},
    {(string_address)"list-columns", 'H'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static string_address ul_lslocks_word(p8 address_to address_to cursor,
                                      p8 address_to stop)
{
        p8 address_to at = address_to cursor;

        while (at < stop && byte_is_space(*at))
                at++;
        if (at == stop)
        {
                address_to cursor = at;
                return null;
        }

        p8 address_to word = at;
        while (at < stop && !byte_is_space(*at))
                at++;
        if (at < stop)
                *at++ = end;
        address_to cursor = at;
        return word;
}

static bool ul_lslocks_id(string_address text, positive address_to value)
{
        string_address at = text;

        return string_digits_checked(address_of at, 10, value) &&
               string_is(at, ':') && !string_get(at + 1);
}

static bool ul_lslocks_device(string_address text,
                              p32 address_to major, p32 address_to minor,
                              positive address_to inode)
{
        string_address at = text;
        positive high;
        positive low;
        positive node;

        if (!string_digits_checked(address_of at, 16, address_of high) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!string_digits_checked(address_of at, 16, address_of low) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!string_digits_checked(address_of at, 10, address_of node) ||
            string_get(at) || high > p32_max || low > p32_max)
                return false;

        address_to major = (p32)high;
        address_to minor = (p32)low;
        address_to inode = node;
        return true;
}

static b32 ul_lslocks_kind(string_address text)
{
        static const string_address kinds[] = {
            (string_address)"POSIX", (string_address)"FLOCK",
            (string_address)"OFDLCK",
        };

        for (positive i = 0; i < array_count(kinds); i++)
                if (string_equals(text, kinds[i]))
                        return (b32)i;
        return -1;
}

static struct snapshot_process address_to ul_lslocks_process(b32 pid)
{
        positive low = 0;
        positive high = ul_lslocks_snapshot.header.process_count;

        while (low < high)
        {
                positive middle = low + (high - low) / 2;
                struct snapshot_process address_to process =
                    ul_lslocks_snapshot.processes + middle;

                if (process->pid < (p32)pid)
                        low = middle + 1;
                else
                        high = middle;
        }

        if (low < ul_lslocks_snapshot.header.process_count &&
            ul_lslocks_snapshot.processes[low].pid == (p32)pid)
                return ul_lslocks_snapshot.processes + low;
        return null;
}

/* Resolve one device/inode through the holder's already-open descriptors.
   There is still one shared PID snapshot and no second walk of /proc itself;
   a disappearing fd or task simply leaves this racing record unresolved. */
static fn ul_lslocks_resolve(ul_lslocks_entry address_to lock)
{
        if (lock->pid < 0)
                return;

        p8 directory[64];
        system_process_path(directory, (p32)lock->pid, null, "fd");
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, directory))
                return;

        struct linux_dirent64 address_to entry;
        while ((entry = file_walk_next(address_of walk)))
        {
                positive fd;
                file_facts facts;

                if (!ul_unsigned(entry->d_name, b32_max, address_of fd) ||
                    !file_look(walk.handle, entry->d_name, 0,
                               address_of facts) ||
                    facts.inode != lock->inode ||
                    facts.device_major != lock->major ||
                    facts.device_minor != lock->minor)
                        continue;

                p8 path[FILE_PATH_MAX];
                bipolar length = system_read_link_at(
                    walk.handle, entry->d_name, path, sizeof(path) - 1);

                lock->size = facts.size;
                lock->size_known = true;

                if (length >= 0 && (positive)length < sizeof(path) - 1)
                {
                        p8 address_to copy = (p8 address_to)text_arena_take(
                            (positive)length + 1);

                        if (copy)
                        {
                                memory_copy_apart_end(copy, path,
                                                     (positive)length);
                                lock->path = copy;
                                lock->accessible = true;
                        }
                }
                break;
        }

        file_walk_close(address_of walk);
}

static string_address ul_lslocks_field(address_any row, p8 column,
                                       p8 address_to scratch)
{
        static const string_address kinds[] = {
            (string_address)"POSIX", (string_address)"FLOCK",
            (string_address)"OFDLCK",
        };
        ul_lslocks_entry address_to lock = (ul_lslocks_entry address_to)row;

        switch (column)
        {
        case UL_LOCKS_COMMAND:
                return lock->process
                           ? (string_address)lock->process->command
                                     : (string_address)"";
        case UL_LOCKS_PID:
                if (lock->pid < 0)
                        return (string_address)"-1";
                positive_into_string(scratch, (positive)(p32)lock->pid);
                return scratch;
        case UL_LOCKS_TYPE:
                return kinds[lock->type];
        case UL_LOCKS_SIZE:
                if (!lock->size_known || !lock->size)
                        return (string_address)"";
                if (ul_lslocks_bytes)
                        positive_into_string(scratch, (positive)lock->size);
                else
                {
                        positive length = positive_into_human_1024_string(
                            scratch, (positive)lock->size);

                        if (length && byte_is_digit(scratch[length - 1]))
                        {
                                scratch[length++] = 'B';
                                scratch[length] = end;
                        }
                }
                return scratch;
        case UL_LOCKS_INODE:
                positive_into_string(scratch, (positive)lock->inode);
                return scratch;
        case UL_LOCKS_DEVICE:
        {
                positive at = positive_into(scratch, lock->major);
                scratch[at++] = ':';
                positive_into_string(scratch + at, lock->minor);
                return scratch;
        }
        case UL_LOCKS_MODE:
                memory_copy_apart_end(scratch,
                                      lock->mode ? (address_any)"WRITE"
                                                 : (address_any)"READ",
                                      lock->mode ? 5 : 4);
                if (lock->blocked)
                {
                        positive at = string_length(scratch);
                        scratch[at++] = '*';
                        scratch[at] = end;
                }
                return scratch;
        case UL_LOCKS_MANDATORY:
                return lock->mandatory ? (string_address)"1"
                                       : (string_address)"0";
        case UL_LOCKS_START:
                positive_into_string(scratch, (positive)lock->start);
                return scratch;
        case UL_LOCKS_END:
                positive_into_string(scratch, (positive)lock->finish);
                return scratch;
        case UL_LOCKS_PATH:
                return lock->path ? lock->path : (string_address)"";
        case UL_LOCKS_BLOCKER:
                if (!lock->blocker)
                        return (string_address)"";
                positive_into_string(scratch, (positive)(p32)lock->blocker);
                return scratch;
        default:
                return (string_address)"";
        }
}

static b32 util_linux_lslocks()
{
        file_taking taking = {
            .program = (string_address)"lslocks",
            .allowed = (string_address)"bJinorupQHVh",
            .valued = (string_address)"opQ",
            .longs = ul_lslocks_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options]", address_of answer))
                return answer;
        if (taking.first != (positive)program_argument_count())
                return ul_bad_usage("lslocks", "unexpected operand");
        if ((taking.flags & FILE_FLAG('J')) &&
            (taking.flags & FILE_FLAG('r')))
                return ul_bad_usage("lslocks",
                                    "--json and --raw are mutually exclusive");
        if (taking.flags & FILE_FLAG('Q'))
                return ul_bad_usage("lslocks", "display filters are not supported");
        if (taking.flags & FILE_FLAG('H'))
                return ul_bad_usage("lslocks",
                                    "column metadata output is not supported");
        if (taking.flags & FILE_FLAG('A'))
                return ul_bad_usage("lslocks", "--output-all is not supported");

        b32 wanted_pid = 0;
        bool pid_selected = file_option_value(address_of taking, 'p') != null;
        if (pid_selected &&
            !ul_pid(file_option_value(address_of taking, 'p'), "lslocks", "PID",
                    address_of wanted_pid))
                return 1;

        static const p8 defaults[] = {
            UL_LOCKS_COMMAND, UL_LOCKS_PID, UL_LOCKS_TYPE, UL_LOCKS_SIZE,
            UL_LOCKS_MODE, UL_LOCKS_MANDATORY, UL_LOCKS_START, UL_LOCKS_END,
            UL_LOCKS_PATH,
        };
        p8 columns[UL_LOCKS_COLUMNS];
        positive column_count = 0;
        string_address output = file_option_value(address_of taking, 'o');

        if (output)
        {
                if (!ul_table_column_list(
                        output, ul_lslocks_columns, UL_LOCKS_COLUMNS, defaults,
                        array_count(defaults), columns,
                        address_of column_count))
                        return ul_bad_usage("lslocks", "unknown output column");
        }
        else
                for (positive i = 0; i < array_count(defaults); i++)
                        columns[column_count++] = defaults[i];

        for (positive i = 0; i < column_count; i++)
                if (columns[i] == UL_LOCKS_HOLDERS)
                        return ul_bad_usage(
                            "lslocks",
                            "HOLDERS requires an unsupported second process-fd census");

        text_begin("lslocks");
        text_arena_used = 0;
        bipolar handle = system_open_at(AT_FDCWD, "/proc/locks",
                                        FILE_READ | O_CLOEXEC);
        if (handle < 0)
                return text_refuse("/proc/locks", "cannot read", 1);

        positive length = 0;
        bool read_failed = false;
        p8 address_to input = text_arena_read_all(
            (positive)handle, 4096, address_of length, address_of read_failed);
        system_close(handle);
        if (!input)
                return text_done(1);

        positive lines = memory_count(input, length, '\n') +
                         (length && input[length - 1] != '\n');
        ul_lslocks_entry address_to locks =
            (ul_lslocks_entry address_to)text_arena_take(
                lines * sizeof(ul_lslocks_entry));
        if (!locks && lines)
                return text_done(1);

        positive count = 0;
        p8 address_to cursor = input;
        p8 address_to input_end = input + length;

        while (cursor < input_end)
        {
                p8 address_to line_end = memory_first_of(
                    cursor, '\n', (positive)(input_end - cursor));
                if (!line_end)
                        line_end = input_end;
                else
                        *line_end = end;

                p8 address_to at = cursor;
                string_address id_text = ul_lslocks_word(address_of at,
                                                         line_end);
                string_address kind_text = ul_lslocks_word(address_of at,
                                                           line_end);
                bool blocked = kind_text && string_equals(kind_text, "->");
                if (blocked)
                        kind_text = ul_lslocks_word(address_of at, line_end);
                string_address scope = ul_lslocks_word(address_of at, line_end);
                string_address mode = ul_lslocks_word(address_of at, line_end);
                string_address pid_text = ul_lslocks_word(address_of at,
                                                          line_end);
                string_address device = ul_lslocks_word(address_of at,
                                                        line_end);
                string_address start_text = ul_lslocks_word(address_of at,
                                                            line_end);
                string_address end_text = ul_lslocks_word(address_of at,
                                                          line_end);
                positive id;
                positive inode;
                positive start;
                positive finish = 0;
                bipolar pid;
                p32 major;
                p32 minor;
                b32 kind = kind_text ? ul_lslocks_kind(kind_text) : -1;

                if (id_text && scope && mode && pid_text && device &&
                    start_text && end_text && kind >= 0 &&
                    ul_lslocks_id(id_text, address_of id) &&
                    ul_signed(pid_text, -1, b32_max, address_of pid) &&
                    ul_lslocks_device(device, address_of major,
                                      address_of minor, address_of inode) &&
                    ul_unsigned(start_text, positive_max,
                                address_of start) &&
                    (string_equals(end_text, "EOF") ||
                     ul_unsigned(end_text, positive_max,
                                 address_of finish)))
                {
                        b32 blocker = 0;

                        if (blocked)
                                for (positive i = count; i; i--)
                                        if (!locks[i - 1].blocked &&
                                            locks[i - 1].id == id)
                                        {
                                                blocker = locks[i - 1].pid;
                                                break;
                                        }

                        locks[count++] = (ul_lslocks_entry){
                            .id = id,
                            .inode = inode,
                            .start = start,
                            .finish = finish,
                            .pid = (b32)pid,
                            .blocker = blocker,
                            .major = major,
                            .minor = minor,
                            .type = (p8)kind,
                            .mode = string_equals(mode, "WRITE"),
                            .mandatory = string_equals(scope, "MANDATORY"),
                            .blocked = blocked,
                        };
                }

                cursor = line_end < input_end ? line_end + 1 : input_end;
        }

        if (!system_snapshot_take(address_of ul_lslocks_snapshot,
                                  SPARK_SNAPSHOT_PROCESS, false))
                return text_refuse("/proc", "cannot read", 1);

        positive shown = 0;
        bool inaccessible = (taking.flags & FILE_FLAG('i')) != 0;
        for (positive i = 0; i < count; i++)
        {
                if (pid_selected && locks[i].pid != wanted_pid)
                        continue;
                locks[i].process = ul_lslocks_process(locks[i].pid);
                ul_lslocks_resolve(locks + i);

                if (inaccessible && !locks[i].accessible)
                        continue;
                locks[shown++] = locks[i];
        }

        ul_lslocks_bytes = (taking.flags & FILE_FLAG('b')) != 0;
        ul_lslocks_columns[UL_LOCKS_SIZE].json = ul_lslocks_bytes
            ? UL_TABLE_NULL_NUMBER : UL_TABLE_NULL_STRING;

        if (taking.flags & FILE_FLAG('J'))
                ul_table_json("locks", locks, sizeof(*locks), shown,
                              ul_lslocks_columns, columns, column_count,
                              ul_lslocks_field);
        else
                ul_table_out(locks, sizeof(*locks), shown,
                             ul_lslocks_columns, UL_LOCKS_COLUMNS, columns,
                             column_count,
                             !(taking.flags & FILE_FLAG('n')),
                             (taking.flags & FILE_FLAG('r')) != 0,
                             ul_lslocks_field);

        log_flush();
        return read_failed ? 1 : 0;
}

// lsfd ------------------------------------------------------------

enum
{
        UL_LSFD_COMMAND,
        UL_LSFD_PID,
        UL_LSFD_USER,
        UL_LSFD_FD,
        UL_LSFD_MODE,
        UL_LSFD_XMODE,
        UL_LSFD_TYPE,
        UL_LSFD_NAME,
        UL_LSFD_KNAME,
        UL_LSFD_INODE,
        UL_LSFD_DEVICE,
        UL_LSFD_MNTID,
        UL_LSFD_SIZE,
        UL_LSFD_POS,
        UL_LSFD_UID,
        UL_LSFD_DELETED,
        UL_LSFD_COLUMNS,
};

static const ul_table_column ul_lsfd_columns[] = {
    {(string_address)"command", (string_address)"COMMAND", 0, false, UL_TABLE_STRING},
    {(string_address)"pid", (string_address)"PID", 5, true, UL_TABLE_NUMBER},
    {(string_address)"user", (string_address)"USER", 0, false, UL_TABLE_STRING},
    {(string_address)"fd", (string_address)"FD", 2, true, UL_TABLE_NUMBER},
    {(string_address)"mode", (string_address)"MODE", 4, false, UL_TABLE_STRING},
    {(string_address)"xmode", (string_address)"XMODE", 6, false, UL_TABLE_STRING},
    {(string_address)"type", (string_address)"TYPE", 5, false, UL_TABLE_STRING},
    {(string_address)"name", (string_address)"NAME", 0, false, UL_TABLE_STRING},
    {(string_address)"kname", (string_address)"KNAME", 0, false, UL_TABLE_STRING},
    {(string_address)"inode", (string_address)"INODE", 5, true, UL_TABLE_NUMBER},
    {(string_address)"maj:min", (string_address)"MAJ:MIN", 7, false, UL_TABLE_STRING},
    {(string_address)"mntid", (string_address)"MNTID", 5, true, UL_TABLE_NULL_NUMBER},
    {(string_address)"size", (string_address)"SIZE", 4, true, UL_TABLE_NUMBER},
    {(string_address)"pos", (string_address)"POS", 3, true, UL_TABLE_NULL_NUMBER},
    {(string_address)"uid", (string_address)"UID", 3, true, UL_TABLE_NUMBER},
    {(string_address)"deleted", (string_address)"DELETED", 7, false, UL_TABLE_BOOLEAN},
};

typedef struct
{
        struct snapshot_process address_to process;
        string_address user;
        string_address name;
        string_address type;
        p64 inode;
        p64 mount_id;
        p64 size;
        p64 position;
        p32 major;
        p32 minor;
        p32 fd;
        p8 access;
        bool access_known;
        bool mount_known;
        bool position_known;
        bool deleted;
} ul_lsfd_entry;

static system_snapshot ul_lsfd_snapshot;
static ul_lsfd_entry address_to ul_lsfd_entries;
static positive ul_lsfd_entry_room;
static positive ul_lsfd_entry_count;
static positive address_to ul_lsfd_pids;
static positive ul_lsfd_pid_room;
static positive ul_lsfd_pid_count;

static const file_long ul_lsfd_longs[] = {
    {(string_address)"threads", 'l'},
    {(string_address)"json", 'J'},
    {(string_address)"noheadings", 'n'},
    {(string_address)"output", 'o'},
    {(string_address)"raw", 'r'},
    {(string_address)"notruncate", 'u'},
    {(string_address)"pid", 'p'},
    {(string_address)"inet", 'i'},
    {(string_address)"filter", 'Q'},
    {(string_address)"debug-filter", 'D'},
    {(string_address)"counter", 'C'},
    {(string_address)"dump-counters", 'd'},
    {(string_address)"hyperlink", 'k'},
    {(string_address)"summary", 's'},
    {(string_address)"list-columns", 'H'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static fn ul_lsfd_release()
{
        array_store_release(ul_lsfd_entries, ul_lsfd_entry_room,
                            ul_lsfd_entry_count);
        array_store_release(ul_lsfd_pids, ul_lsfd_pid_room,
                            ul_lsfd_pid_count);
}

/* fdinfo is deliberately read from the holder already admitted by the shared
   process snapshot. Parse its three small scalar records in place, without a
   second proc walker or a per-descriptor general-purpose parser. */
static bool ul_lsfd_fdinfo_value(p8 address_to bytes, positive length,
                                 string_address field, positive base,
                                 positive address_to value)
{
        positive field_length = string_length(field);
        positive at = 0;

        while (at < length)
        {
                positive finish = at;

                while (finish < length && bytes[finish] != '\n')
                        finish++;
                if (finish - at > field_length &&
                    !memory_compare(bytes + at, field, field_length))
                {
                        positive cursor = at + field_length;
                        positive made = 0;
                        bool any = false;

                        while (cursor < finish && byte_is_space(bytes[cursor]))
                                cursor++;
                        while (cursor < finish)
                        {
                                positive digit = digit_known(bytes[cursor], base);

                                if (digit >= base ||
                                    made > (positive_max - digit) / base)
                                        return false;
                                made = made * base + digit;
                                any = true;
                                cursor++;
                        }

                        if (any)
                        {
                                address_to value = made;
                                return true;
                        }
                        return false;
                }
                at = finish < length ? finish + 1 : length;
        }
        return false;
}

static fn ul_lsfd_fdinfo(ul_lsfd_entry address_to descriptor)
{
        p8 path[FILE_PATH_MAX];
        p8 bytes[1024];

        system_process_path(path, descriptor->process->pid, "fdinfo", "");
        positive at = string_length(path);
        positive_into_string(path + at, descriptor->fd);

        bipolar got = file_slurp_once_at(AT_FDCWD, path, bytes, sizeof(bytes));
        if (got <= 0)
                return;

        positive value;
        if (ul_lsfd_fdinfo_value(bytes, (positive)got, "flags:", 8,
                                 address_of value))
        {
                descriptor->access = (p8)(value & 3);
                descriptor->access_known = descriptor->access <= 2;
        }
        if (ul_lsfd_fdinfo_value(bytes, (positive)got, "pos:", 10,
                                 address_of value))
        {
                descriptor->position = value;
                descriptor->position_known = true;
        }
        if (ul_lsfd_fdinfo_value(bytes, (positive)got, "mnt_id:", 10,
                                 address_of value))
        {
                descriptor->mount_id = value;
                descriptor->mount_known = true;
        }
}

static PURE string_address ul_lsfd_type(p16 mode, string_address name)
{
        if (name && string_length(name) >= 11 &&
            !memory_compare(name, "anon_inode:", 11))
                return (string_address)"anon_inode";

        switch (mode & MODE_FORMAT)
        {
        case MODE_FILE:      return (string_address)"REG";
        case MODE_DIRECTORY: return (string_address)"DIR";
        case MODE_PIPE:      return (string_address)"FIFO";
        case MODE_SOCKET:    return (string_address)"SOCK";
        case MODE_CHARACTER: return (string_address)"CHR";
        case MODE_BLOCK:     return (string_address)"BLK";
        default:             return (string_address)"UNKN";
        }
}

/* One means copied, minus one is an ordinary procfs race, and zero is the
   arena refusing the retained name. */
static bipolar ul_lsfd_copy_name(ul_lsfd_entry address_to descriptor,
                                 bipolar directory, string_address fd)
{
        p8 path[FILE_PATH_MAX];
        bipolar length = system_read_link_at(directory, fd, path,
                                              sizeof(path) - 1);

        if (length < 0 || (positive)length >= sizeof(path) - 1)
                return -1;

        p8 address_to copy = (p8 address_to)text_arena_take(
            (positive)length + 1);
        if (!copy)
                return 0;
        memory_copy_apart_end(copy, path, (positive)length);
        descriptor->name = copy;
        descriptor->deleted = length >= 10 &&
            !memory_compare(copy + length - 10, " (deleted)", 10);
        return 1;
}

static bool ul_lsfd_process(struct snapshot_process address_to process)
{
        p8 directory[64];
        file_walk walk;
        string_address user = null;

        system_process_path(directory, process->pid, null, "fd");
        if (!file_walk_open(address_of walk, AT_FDCWD, directory))
                return true;

        struct linux_dirent64 address_to dirent;
        while ((dirent = file_walk_next(address_of walk)))
        {
                positive fd;
                file_facts facts;

                if (!ul_unsigned(dirent->d_name, p32_max, address_of fd) ||
                    !file_look(walk.handle, dirent->d_name, 0,
                               address_of facts))
                        continue;

                if (!array_store_reserve(ul_lsfd_entries, ul_lsfd_entry_room,
                                         ul_lsfd_entry_count,
                                         ul_lsfd_entry_count + 1, 64))
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                ul_lsfd_entry address_to descriptor =
                    ul_lsfd_entries + ul_lsfd_entry_count;
                memory_fill(descriptor, 0, sizeof(*descriptor));
                descriptor->process = process;
                descriptor->inode = facts.inode;
                descriptor->mount_id = facts.mount_id;
                descriptor->mount_known = facts.mount_id != 0;
                descriptor->size = facts.size;
                bool device = (facts.mode & MODE_FORMAT) == MODE_CHARACTER ||
                              (facts.mode & MODE_FORMAT) == MODE_BLOCK;
                descriptor->major = device ? facts.rdev_major
                                           : facts.device_major;
                descriptor->minor = device ? facts.rdev_minor
                                           : facts.device_minor;
                descriptor->fd = (p32)fd;

                bipolar named = ul_lsfd_copy_name(descriptor, walk.handle,
                                                  dirent->d_name);
                if (named <= 0)
                {
                        if (!named)
                        {
                                file_walk_close(address_of walk);
                                return false;
                        }
                        continue;
                }

                descriptor->type = ul_lsfd_type(facts.mode,
                                                 descriptor->name);
                ul_lsfd_fdinfo(descriptor);
                if (!user)
                        user = ps_name_of(process->uid);
                if (!user)
                {
                        file_walk_close(address_of walk);
                        return false;
                }
                descriptor->user = user;
                ul_lsfd_entry_count++;
        }

        file_walk_close(address_of walk);
        return true;
}

static PURE bipolar ul_lsfd_order(ul_lsfd_entry left, ul_lsfd_entry right)
{
        if (left.process->pid != right.process->pid)
                return left.process->pid < right.process->pid ? -1 : 1;
        if (left.fd != right.fd)
                return left.fd < right.fd ? -1 : 1;
        return 0;
}

static string_address ul_lsfd_field(address_any row, p8 column,
                                    p8 address_to scratch)
{
        ul_lsfd_entry address_to descriptor = (ul_lsfd_entry address_to)row;

        switch (column)
        {
        case UL_LSFD_COMMAND:
                return descriptor->process->command;
        case UL_LSFD_PID:
                positive_into_string(scratch, descriptor->process->pid);
                return scratch;
        case UL_LSFD_USER:
                return descriptor->user;
        case UL_LSFD_FD:
                positive_into_string(scratch, descriptor->fd);
                return scratch;
        case UL_LSFD_MODE:
                if (!descriptor->access_known)
                        return (string_address)"";
                return descriptor->access == 0 ? (string_address)"r--"
                     : descriptor->access == 1 ? (string_address)"-w-"
                                               : (string_address)"rw-";
        case UL_LSFD_XMODE:
                if (!descriptor->access_known)
                        return (string_address)"";
                return descriptor->access == 0 ? (string_address)"r-----"
                     : descriptor->access == 1 ? (string_address)"-w----"
                                               : (string_address)"rw----";
        case UL_LSFD_TYPE:
                return descriptor->type;
        case UL_LSFD_NAME:
        case UL_LSFD_KNAME:
                return descriptor->name;
        case UL_LSFD_INODE:
                positive_into_string(scratch, (positive)descriptor->inode);
                return scratch;
        case UL_LSFD_DEVICE:
        {
                positive at = positive_into_string(scratch, descriptor->major);
                scratch[at++] = ':';
                positive_into_string(scratch + at, descriptor->minor);
                return scratch;
        }
        case UL_LSFD_MNTID:
                if (!descriptor->mount_known)
                        return (string_address)"";
                positive_into_string(scratch, (positive)descriptor->mount_id);
                return scratch;
        case UL_LSFD_SIZE:
                positive_into_string(scratch, (positive)descriptor->size);
                return scratch;
        case UL_LSFD_POS:
                if (!descriptor->position_known)
                        return (string_address)"";
                positive_into_string(scratch, (positive)descriptor->position);
                return scratch;
        case UL_LSFD_UID:
                positive_into_string(scratch, descriptor->process->uid);
                return scratch;
        default:
                return descriptor->deleted ? (string_address)"1"
                                           : (string_address)"0";
        }
}

static b32 util_linux_lsfd()
{
        file_taking taking = {
            .program = (string_address)"lsfd",
            .allowed = (string_address)"lJnorupiQDCHVh",
            .valued = (string_address)"opQC",
            .optional = (string_address)"i",
            .long_optional = (string_address)"ks",
            .longs = ul_lsfd_longs,
        };
        b32 answer;

        ul_lsfd_release();
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options]", address_of answer))
                return answer;
        if (taking.first != (positive)program_argument_count())
                return ul_bad_usage("lsfd", "unexpected operand");
        if ((taking.flags & FILE_FLAG('J')) &&
            (taking.flags & FILE_FLAG('r')))
                return ul_bad_usage("lsfd",
                                    "--json and --raw are mutually exclusive");
        if (taking.flags & FILE_FLAG('l'))
                return ul_bad_usage("lsfd", "thread descriptors are not supported");
        if (taking.flags & FILE_FLAG('i'))
                return ul_bad_usage("lsfd", "network endpoint filtering is not supported");
        if (taking.flags & FILE_FLAG('Q'))
                return ul_bad_usage("lsfd", "display filters are not supported");
        if (taking.flags & FILE_FLAG('D'))
                return ul_bad_usage("lsfd", "filter debugging is not supported");
        if ((taking.flags & FILE_FLAG('C')) ||
            (taking.flags & FILE_FLAG('d')) ||
            (taking.flags & FILE_FLAG('s')))
                return ul_bad_usage("lsfd", "descriptor counters are not supported");
        if (taking.flags & FILE_FLAG('k'))
                return ul_bad_usage("lsfd", "terminal hyperlinks are not supported");
        if (taking.flags & FILE_FLAG('H'))
                return ul_bad_usage("lsfd", "column metadata output is not supported");

        static const p8 defaults[] = {
            UL_LSFD_COMMAND, UL_LSFD_PID, UL_LSFD_USER, UL_LSFD_FD,
            UL_LSFD_MODE, UL_LSFD_TYPE, UL_LSFD_INODE, UL_LSFD_NAME,
        };
        p8 columns[UL_LSFD_COLUMNS];
        positive column_count = 0;
        string_address output = file_option_value(address_of taking, 'o');

        if (output)
        {
                if (!ul_table_column_list(
                        output, ul_lsfd_columns, UL_LSFD_COLUMNS, defaults,
                        array_count(defaults), columns,
                        address_of column_count))
                        return ul_bad_usage("lsfd", "unknown output column");
        }
        else
                for (positive i = 0; i < array_count(defaults); i++)
                        columns[column_count++] = defaults[i];

        text_begin("lsfd");
        text_arena_used = 0;

        string_address pid_list = file_option_value(address_of taking, 'p');
        if (pid_list &&
            !ps_pid_list(pid_list, address_of ul_lsfd_pids,
                         address_of ul_lsfd_pid_count,
                         address_of ul_lsfd_pid_room, false))
        {
                ul_lsfd_release();
                return text_refuse(pid_list, "invalid PID list", 1);
        }

        if (!system_snapshot_take(address_of ul_lsfd_snapshot,
                                  SPARK_SNAPSHOT_PROCESS, true))
        {
                ul_lsfd_release();
                return text_refuse("/proc", "cannot read", 1);
        }

        bool failed = false;
        for (positive i = 0; i < ul_lsfd_snapshot.header.process_count; i++)
        {
                struct snapshot_process address_to process =
                    ul_lsfd_snapshot.processes + i;

                if (ul_lsfd_pid_count &&
                    !ps_value_has(ul_lsfd_pids, ul_lsfd_pid_count,
                                  process->pid))
                        continue;
                if (!ul_lsfd_process(process))
                {
                        failed = true;
                        break;
                }
        }

        ul_lsfd_entry address_to rows = ul_lsfd_entries;
        if (!failed && ul_lsfd_entry_count)
        {
                ul_lsfd_entry address_to spare =
                    (ul_lsfd_entry address_to)text_arena_take(
                        ul_lsfd_entry_count * sizeof(*spare));
                if (!spare)
                        failed = true;
                else
                        rows = array_merge_sort(rows, spare,
                                                ul_lsfd_entry_count,
                                                ul_lsfd_order);
        }

        if (!failed)
        {
                if (taking.flags & FILE_FLAG('J'))
                        ul_table_json("lsfd", rows, sizeof(*rows),
                                      ul_lsfd_entry_count, ul_lsfd_columns,
                                      columns, column_count, ul_lsfd_field);
                else
                        ul_table_out(rows, sizeof(*rows), ul_lsfd_entry_count,
                                     ul_lsfd_columns, UL_LSFD_COLUMNS, columns,
                                     column_count,
                                     !(taking.flags & FILE_FLAG('n')),
                                     (taking.flags & FILE_FLAG('r')) != 0,
                                     ul_lsfd_field);
                log_flush();
        }

        ul_lsfd_release();
        return failed ? text_done(1) : 0;
}

static bipolar ul_directory_open_at(string_address program, bipolar base,
                                    string_address path)
{
        bipolar handle = system_open_at(base,
                                        path,
                                        FILE_READ | O_DIRECTORY | O_CLOEXEC);
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
        bipolar error = ul_path_write(path, bytes, length);

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

        if (!string_digits_checked(address_of at, 10, address_of inside) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!string_digits_checked(address_of at, 10, address_of outside) ||
            !string_is(at, ':'))
                return false;
        at++;
        if (!string_digits_checked(address_of at, 10, address_of count) ||
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
        bipolar value = ul_identity(text, (positive)p32_max - 1, group);

        if (value < 0)
                return false;

        address_to id = (positive)value;
        return true;
}

static b32 ul_namespace_identity(string_address program,
                                 string_address uid_text,
                                 string_address gid_text,
                                 positive uid, positive gid,
                                 p8 following,
                                 bool uid_root, bool gid_root,
                                 bipolar follow_handle, bool groups_cleared)
{
        file_facts facts;

        if (following &&
            !file_look(follow_handle, "", AT_EMPTY_PATH, address_of facts))
                return ul_bad_usage(program, "cannot follow target identity");
        if (following & 1) uid = facts.owner;
        if (following & 2) gid = facts.group;

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

static b32 ul_namespace_wait(bipolar child, bool job_control)
{
        positive status = 0;

        process_signal_default(SIGCHLD);
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

                        log_flush();
                        process_signal_default(signal);
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
                if (file_account_name(file_account_text(FILE_ACCOUNT_USER), uid,
                                      2, account,
                                      sizeof(account)))
                while (file_account_next(file_account_text(FILE_ACCOUNT_USER),
                                         address_of at,
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

        /* The login spelling upstream's exec_shell builds: a dash before the
           shell's basename, which is what makes it read its profile. */
        p8 name[FILE_NAME_MAX + 1];
        string_address slash = string_last_of_or_end(shell, '/');

        name[0] = '-';
        string_copy_max_end(name + 1, string_get(slash) ? slash + 1 : shell,
                            sizeof(name) - 2);

        string_address words[] = {name, null};
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
        bipolar handle = system_open_at(AT_FDCWD,
                                        path,
                                        FILE_READ | O_CLOEXEC);

        if (handle >= 0)
                system_close(handle);
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

        process_signal_default(SIGCHLD);
        log_flush();
        bipolar child = system_fork();
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
        system_process_path(path, (p32)target, null,
                            group ? (string_address)"gid_map"
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

        if (system_pipe(address_of ready,
                          O_CLOEXEC) < 0)
                return ul_bad_usage("unshare", "cannot make mapping channel");

        b32 target = (b32)system_call(syscall(getpid));
        process_signal_default(SIGCHLD);
        log_flush();
        bipolar helper = system_fork();
        if (helper < 0)
        {
                system_close(ready[0]);
                system_close(ready[1]);
                return ul_bad_usage("unshare", "fork failed");
        }

        p8 byte = 1;
        if (!helper)
        {
                system_close(ready[1]);
                bool failed = system_read_retry((positive)ready[0],
                                                address_of byte, 1) != 1;
                p8 path[64];

                if (!failed && map->deny_groups)
                {
                        system_process_path(path, (p32)target, null,
                                            "setgroups");
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
                system_close(ready[0]);
                log_flush();
                system_call_1(syscall(exit), failed);
                return 1;
        }

        system_close(ready[0]);
        bool failed = system_call_1(syscall(unshare), CLONE_NEWUSER) < 0;
        if (failed)
                ul_bad_usage("unshare", "unshare failed");
        if (!failed)
                failed = system_write_all((positive)ready[1],
                                          address_of byte, 1) != 1;
        system_close(ready[1]);
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
        if (system_pipe(address_of channel,
                          O_CLOEXEC) < 0)
                return false;
        process_signal_default(SIGCHLD);
        log_flush();
        state->child = system_fork();
        if (state->child < 0)
        {
                system_close(channel[0]);
                system_close(channel[1]);
                return false;
        }
        if (!state->child)
        {
                p8 go;
                bool failed;
                b32 target = (b32)system_call(syscall(getppid));

                system_close(channel[1]);
                failed = system_read_retry((positive)channel[0],
                                           address_of go, 1) != 1;
                system_close(channel[0]);
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
                        system_process_path(source, (p32)target,
                                            "ns", name);
                        if (system_mount(source, destination, 0, MS_BIND, 0) < 0)
                        {
                                file_fail("unshare: cannot bind namespace file\n",
                                          0);
                                failed = true;
                        }
                }
                log_flush();
                system_call_1(syscall(exit), failed);
        }
        system_close(channel[0]);
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
        system_close(state->notify);
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
        positive uid = 0;
        positive gid = 0;
        if ((set_uid && (string_equals(set_uid, "follow") ||
                         !ul_unsigned(set_uid, (positive)p32_max - 1,
                                      address_of uid))) ||
            (set_gid && (string_equals(set_gid, "follow") ||
                         !ul_unsigned(set_gid, (positive)p32_max - 1,
                                      address_of gid))))
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
                    map.gid_range_count || map.deny_groups)
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
            system_mount(0, "/", 0,
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

                if (system_signal_mask(UL_SIGNAL_BLOCK, address_of blocked,
                                       address_of old, 8) < 0)
                        return ul_bad_usage("unshare", "cannot block signals");
                process_signal_default(SIGCHLD);
                log_flush();
                bipolar child = system_fork();
                if (child < 0)
                {
                        system_signal_mask(UL_SIGNAL_SET_MASK,
                                           address_of old, 0, 8);
                        return ul_bad_usage("unshare", "fork failed");
                }
                if (child > 0)
                {
                        answer = ul_namespace_wait(child, false);
                        system_signal_mask(UL_SIGNAL_SET_MASK,
                                           address_of old, 0, 8);
                        return answer;
                }
                system_signal_mask(UL_SIGNAL_SET_MASK, address_of old, 0, 8);
        }

        string_address root = file_option_value(address_of taking, 'R');
        string_address wd = file_option_value(address_of taking, 'w');
        if (root && (system_change_directory(root) < 0 ||
                     system_call_1(syscall(chroot), (positive)".") < 0 ||
                     system_change_directory("/") < 0))
                return ul_bad_usage("unshare", "cannot change root");
        if (wd && system_change_directory(wd) < 0)
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
                if (system_mount("proc", target, "proc",
                                  MS_NOSUID | MS_NODEV | MS_NOEXEC, 0) < 0)
                        return ul_bad_usage("unshare", "cannot mount proc");
        }

        /* Supplementary groups go with the identity change, as upstream
           orders it: before the helper has written the id maps the kernel
           refuses setgroups in the new user namespace outright. */
        if (ul_namespace_identity("unshare", set_uid, set_gid, uid, gid, 0,
                                  uid_choice == 'r', gid_choice == 'r', -1,
                                  false))
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
                system_process_path(target_path, (p32)target, null,
                                    "");
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
                {
                        ul_bad_usage("nsenter", "target PID is required");
                        goto nsenter_failed;
                }
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
        positive uid_id = 0;
        positive gid_id = 0;
        bool follow_uid = uid && string_equals(uid, "follow");
        bool follow_gid = gid && string_equals(gid, "follow");
        if ((uid && !follow_uid &&
             !ul_unsigned(uid, (positive)p32_max - 1, address_of uid_id)) ||
            (gid && !follow_gid &&
             !ul_unsigned(gid, (positive)p32_max - 1, address_of gid_id)))
        {
                ul_bad_usage("nsenter", "invalid user or group");
                goto nsenter_failed;
        }
        if ((follow_uid || follow_gid) && target_handle < 0)
        {
                ul_bad_usage("nsenter", "target PID is required");
                goto nsenter_failed;
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
                        system_close(handles[at]);
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
                system_close(handles[which]);
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
            system_change_directory(wdns) < 0)
        {
                ul_bad_usage("nsenter", "cannot change directory");
                goto nsenter_failed;
        }
        if (root_handle >= 0) system_close(root_handle);
        if (wd_handle >= 0) system_close(wd_handle);
        if (old_cwd >= 0) system_close(old_cwd);
        root_handle = wd_handle = old_cwd = -1;

        if ((uid || gid || (!preserve && entered_user)) &&
            ul_namespace_identity("nsenter", uid, gid, uid_id, gid_id,
                                  follow_uid | (follow_gid << 1),
                                  !preserve && entered_user,
                                  !preserve && entered_user, target_handle,
                                  groups_cleared))
                goto nsenter_failed;
        if (target_handle >= 0)
        {
                system_close(target_handle);
                target_handle = -1;
        }

        if (entered_pid && !(taking.flags & FILE_FLAG('F')))
        {
                process_signal_default(SIGCHLD);
                log_flush();
                bipolar child = system_fork();
                if (child < 0)
                        return ul_bad_usage("nsenter", "fork failed");
                if (child > 0)
                        return ul_namespace_wait(child, true);
        }

        return taking.first < count
            ? ul_exec(taking.first, "nsenter")
            : ul_exec_shell("nsenter");

nsenter_failed:
        if (target_handle >= 0) system_close(target_handle);
        if (root_handle >= 0) system_close(root_handle);
        if (wd_handle >= 0) system_close(wd_handle);
        if (old_cwd >= 0) system_close(old_cwd);
        for (positive at = 0; at < UL_NS_COUNT; at++)
                if (handles[at] >= 0)
                        system_close(handles[at]);
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
                child = system_fork();
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
            system_control(0, UL_TIOCSCTTY, 1) < 0)
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
                bipolar handle = system_open_at(AT_FDCWD,
                                                "/dev/tty",
                                                FILE_READ | O_CLOEXEC);

                /* Upstream deliberately ignores an absent controlling tty. */
                if (handle >= 0)
                {
                        positive blocked = (positive)1 << (UL_SIGNAL_TTOU - 1);
                        positive old = 0;
                        bipolar group = system_call_1(syscall(getpgid), 0);

                        if (system_signal_mask(UL_SIGNAL_BLOCK,
                                               address_of blocked,
                                               address_of old, 8) < 0 ||
                            group < 0 ||
                            system_control(handle, UL_TIOCSPGRP,
                                           address_of group) < 0)
                        {
                                system_close(handle);
                                return ul_bad_usage("setpgid",
                                                    "cannot set foreground process group");
                        }

                        system_signal_mask(UL_SIGNAL_SET_MASK,
                                           address_of old, 0, 8);
                        system_close(handle);
                }
        }

        return ul_exec(taking.first, "setpgid");
}

static const file_long ul_fallocate_longs[] = {
    {(string_address)"collapse-range", 'c'},
    {(string_address)"insert-range", 'i'},
    {(string_address)"keep-size", 'n'},
    {(string_address)"length", 'l'},
    {(string_address)"offset", 'o'},
    {(string_address)"posix", 'x'},
    {(string_address)"punch-hole", 'p'},
    {(string_address)"write-zeroes", 'w'},
    {(string_address)"zero-range", 'z'},
    {(string_address)"verbose", 'v'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

/*
        The useful fallocate modes are already one kernel operation.  Keeping
        the applet as a syscall front matters for image construction: no zero
        buffer is allocated, dirtied or copied merely to reserve an extent.
        --dig-holes and --report-holes are scanners rather than allocation
        operations and remain deliberately unclaimed.
*/
static b32 util_linux_fallocate()
{
        file_taking taking = {
            .program = (string_address)"fallocate",
            .allowed = (string_address)"cilnopvwxzVh",
            .valued = (string_address)"lo",
            .longs = ul_fallocate_longs,
        };
        positive count = (positive)program_argument_count();
        positive length;
        positive offset = 0;
        positive flags;
        positive operations;
        positive mode = 0;
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] filename", address_of answer))
                return answer;
        if (taking.first >= count)
                return ul_bad_usage("fallocate", "no filename specified");
        if (taking.first + 1 != count)
                return ul_bad_usage("fallocate", "unexpected number of arguments");
        if (!file_option_value(address_of taking, 'l'))
                return ul_bad_usage("fallocate", "no length argument specified");
        if (!ul_size(file_option_value(address_of taking, 'l'),
                     address_of length) || !length || length > (positive)b64_max)
                return ul_bad_usage("fallocate", "invalid length");
        if (file_option_value(address_of taking, 'o') &&
            (!ul_size(file_option_value(address_of taking, 'o'),
                      address_of offset) || offset > (positive)b64_max))
                return ul_bad_usage("fallocate", "invalid offset");

        flags = taking.flags;
        operations = ((flags & FILE_FLAG('c')) != 0) +
                     ((flags & FILE_FLAG('i')) != 0) +
                     ((flags & FILE_FLAG('p')) != 0) +
                     ((flags & FILE_FLAG('w')) != 0) +
                     ((flags & FILE_FLAG('x')) != 0) +
                     ((flags & FILE_FLAG('z')) != 0);

        if (operations > 1 ||
            ((flags & FILE_FLAG('n')) &&
             (flags & (FILE_FLAG('c') | FILE_FLAG('i') | FILE_FLAG('w') |
                       FILE_FLAG('x')))))
                return ul_bad_usage("fallocate", "mutually exclusive options");

        if (flags & FILE_FLAG('c'))
                mode = UL_FALLOC_COLLAPSE_RANGE;
        else if (flags & FILE_FLAG('i'))
                mode = UL_FALLOC_INSERT_RANGE;
        else if (flags & FILE_FLAG('p'))
                mode = UL_FALLOC_PUNCH_HOLE | UL_FALLOC_KEEP_SIZE;
        else if (flags & FILE_FLAG('w'))
                mode = UL_FALLOC_WRITE_ZEROES;
        else if (flags & FILE_FLAG('z'))
                mode = UL_FALLOC_ZERO_RANGE;

        if (flags & FILE_FLAG('n'))
                mode |= UL_FALLOC_KEEP_SIZE;

        string_address path = program_argument((b32)taking.first);
        bool create = !(mode & ~UL_FALLOC_WRITE_ZEROES);
        bipolar handle = system_open_at_mode(
            AT_FDCWD, path,
            FILE_READ_WRITE | (create ? FILE_CREATE : 0), 0666);

        if (handle < 0)
        {
                string_format(file_fail, "fallocate: cannot open %s: %s\n",
                              path, file_reason(handle));
                return 1;
        }

        bipolar done = system_call_4(syscall(fallocate), (positive)handle,
                                     mode, offset, length);
        bipolar closed = system_close(handle);

        if (done < 0)
        {
                string_format(file_fail, "fallocate: fallocate failed: %s\n",
                              file_reason(done));
                return 1;
        }
        if (closed < 0)
        {
                string_format(file_fail, "fallocate: write failed: %s\n", path);
                return 1;
        }

        if (flags & FILE_FLAG('v'))
        {
                p8 human[16];
                positive human_length =
                    positive_into_human_nearest_string(human, length, true);

                /* util-linux omits a meaningless trailing decimal zero: its
                   8192-byte spelling is "8 KiB", while the shared nearest
                   formatter intentionally says "8.0 KiB" for dd. */
                for (positive at = 0; at + 2 < human_length; at++)
                        if (human[at] == '.' && human[at + 1] == '0' &&
                            human[at + 2] == ' ')
                        {
                                memory_copy_apart(human + at, human + at + 2,
                                                  human_length - at - 1);
                                break;
                        }

                string_format(log, "%s: %s (", path, human);
                positive_to_string(log, length);
                log(" bytes) ", 0);

                if (mode & UL_FALLOC_PUNCH_HOLE)
                        log("hole created.\n", 0);
                else if (mode & UL_FALLOC_COLLAPSE_RANGE)
                        log("removed.\n", 0);
                else if (mode & UL_FALLOC_INSERT_RANGE)
                        log("inserted.\n", 0);
                else if (mode & UL_FALLOC_ZERO_RANGE)
                        log("zeroed.\n", 0);
                else if (mode & UL_FALLOC_WRITE_ZEROES)
                        log("written as zeroes.\n", 0);
                else
                        log("allocated.\n", 0);
        }

        return 0;
}

static const file_long ul_copyfilerange_longs[] = {
    {(string_address)"verbose", 'v'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

/* source-offset:destination-offset:length, with an empty offset continuing
   from the previous range. The kernel updates both explicit offset words, so
   a list of ranges does not need an lseek before or after each operation. */
static bool ul_copyfilerange_range(string_address written, bipolar in,
                                   bipolar out, p64 source_size,
                                   p64 address_to in_offset,
                                   p64 address_to out_offset, bool verbose,
                                   string_address source,
                                   string_address destination)
{
        p8 copy[FILE_PATH_MAX];
        positive length = string_length(written);
        string_address first;
        string_address second;
        positive parsed;

        if (length >= sizeof(copy))
                return false;

        memory_copy_apart_end(copy, written, length);
        first = string_first_of(copy, ':');
        if (!first)
                return false;
        first[0] = end;

        second = string_first_of(first + 1, ':');
        if (!second)
                return false;
        second[0] = end;

        if (string_get(copy))
        {
                if (!ul_size(copy, address_of parsed) ||
                    parsed > (positive)b64_max)
                        return false;
                address_to in_offset = parsed;
        }

        if (string_get(first + 1))
        {
                if (!ul_size(first + 1, address_of parsed) ||
                    parsed > (positive)b64_max)
                        return false;
                address_to out_offset = parsed;
        }

        if (string_get(second + 1))
        {
                if (!ul_size(second + 1, address_of parsed) ||
                    parsed > (positive)b64_max)
                        return false;
                length = parsed;
        }
        else
                length = 0;

        if (address_to in_offset > source_size)
                return false;
        if (!length)
                length = (positive)(source_size - address_to in_offset);

        while (length)
        {
                positive chunk = length > FILE_KERNEL_COPY_SIZE
                                     ? FILE_KERNEL_COPY_SIZE : length;

                if (verbose)
                {
                        string_format(log, "copy_file_range %s to %s ",
                                      source, destination);
                        positive_to_string(log, address_to in_offset);
                        log(":", 1);
                        positive_to_string(log, address_to out_offset);
                        log(":", 1);
                        positive_to_string(log, chunk);
                        log("\n", 1);
                }

                bipolar copied = file_copy_range_once(
                    in, in_offset, out, out_offset, chunk);

                if (copied == UL_ERROR_INTERRUPTED)
                        continue;
                if (copied < 0)
                {
                        string_format(file_fail,
                                      "copyfilerange: failed to copy range: %s\n",
                                      file_reason(copied));
                        return false;
                }
                if (!copied)
                        break;

                length -= (positive)copied;
        }

        return true;
}

static b32 util_linux_copyfilerange()
{
        file_taking taking = {
            .program = (string_address)"copyfilerange",
            .allowed = (string_address)"vVh",
            .longs = ul_copyfilerange_longs,
        };
        positive count = (positive)program_argument_count();
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] source destination range...",
                    address_of answer))
                return answer;
        if (count - taking.first < 3)
                return ul_bad_usage("copyfilerange", "too few arguments");

        string_address source = program_argument((b32)taking.first++);
        string_address destination = program_argument((b32)taking.first++);
        file_facts facts;
        bipolar in = system_open_at(AT_FDCWD, source, FILE_READ);

        if (in < 0)
        {
                string_format(file_fail,
                              "copyfilerange: cannot open source %s: %s\n",
                              source, file_reason(in));
                return 1;
        }
        if (!file_look(in, (string_address)"", AT_EMPTY_PATH,
                       address_of facts))
        {
                system_close(in);
                return ul_bad_usage("copyfilerange",
                                    "cannot determine source size");
        }

        bipolar out = system_open_at_mode(
            AT_FDCWD, destination, FILE_WRITE & ~O_TRUNC, 0666);

        if (out < 0)
        {
                string_format(file_fail,
                              "copyfilerange: cannot open destination %s: %s\n",
                              destination, file_reason(out));
                system_close(in);
                return 1;
        }

        p64 in_offset = 0;
        p64 out_offset = 0;
        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        bool complete = true;

        while (taking.first < count)
        {
                string_address range = program_argument((b32)taking.first++);

                if (!ul_copyfilerange_range(range, in, out, facts.size,
                                             address_of in_offset,
                                             address_of out_offset, verbose,
                                             source, destination))
                {
                        string_format(file_fail,
                                      "copyfilerange: invalid range: %s\n",
                                      range);
                        complete = false;
                        break;
                }
        }

        system_close(in);
        system_close(out);
        return complete ? 0 : 1;
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
                                           array_count(names));

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

                handle = system_open_at(
                    AT_FDCWD, program_argument((b32)taking.first), FILE_READ);
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
                system_close(handle);

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
             at < array_count(ul_ionice_classes); at++)
                if (file_same_word(text, length, ul_ionice_classes[at]))
                        return (b32)at;

        return -1;
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
                answer = !ul_pid(
                    file_option_value(address_of taking, ul_ionice_identity),
                    "ionice", id_kind, address_of id);
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
                        if (!ul_pid(program_argument((b32)at), "ionice",
                                    id_kind, address_of id))
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

// choom -----------------------------------------------------------
static const file_long ul_choom_longs[] = {
    {"adjust", 'n'}, {"pid", 'p'}, {"help", 'h'}, {"version", 'V'},
    {null, 0},
};

static bool ul_choom_read(string_address path, b32 address_to value)
{
        p8 text[32];
        bipolar parsed;

        if (ul_slurp_word(path, text, sizeof(text)) <= 0 ||
            !ul_signed(text, b32_min, b32_max, address_of parsed))
                return false;

        address_to value = (b32)parsed;
        return true;
}

static bool ul_choom_write(string_address path, b32 value)
{
        p8 text[32];
        positive length = bipolar_into_string(text, value);

        text[length++] = '\n';
        return ul_path_write(path, text, length) == 0;
}

static b32 util_linux_choom()
{
        file_taking taking = {
            .program = "choom", .allowed = "nphV", .valued = "np",
            .longs = ul_choom_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking,
                    "[options] -p PID | -n NUMBER command [argument ...]",
                    address_of answer))
                return answer;

        positive count = (positive)program_argument_count();
        string_address pid_text = file_option_value(address_of taking, 'p');
        string_address adjustment = file_option_value(address_of taking, 'n');
        b32 pid = 0;
        bipolar parsed = 0;

        if (pid_text &&
            (!ul_pid(pid_text, "choom", "PID", address_of pid) || !pid))
                return 1;
        if (adjustment &&
            !ul_signed(adjustment, b32_min, b32_max, address_of parsed))
                return ul_bad_usage("choom", "invalid adjust argument");
        if (pid && taking.first < count)
                return ul_bad_usage("choom", "PID and command are mutually exclusive");
        if (!pid && taking.first >= count)
                return ul_bad_usage("choom", "no PID or COMMAND specified");
        if (!adjustment && taking.first < count)
                return ul_bad_usage("choom", "no OOM score adjust value specified");

        b32 target = pid ? pid : (b32)system_call(syscall(getpid));
        p8 score_path[64];
        p8 adjust_path[64];

        system_process_path(score_path, (p32)target, null,
                            "oom_score");
        system_process_path(adjust_path, (p32)target, null,
                            "oom_score_adj");

        if (!adjustment)
        {
                b32 score;
                b32 old;

                if (!ul_choom_read(score_path, address_of score) ||
                    !ul_choom_read(adjust_path, address_of old))
                        return ul_bad_usage("choom", "failed to read OOM score");
                string_format(log,
                              "pid %b's current OOM score: %b\n"
                              "pid %b's current OOM score adjust value: %b\n",
                              (bipolar)pid, (bipolar)score,
                              (bipolar)pid, (bipolar)old);
                log_flush();
                return 0;
        }

        b32 old = 0;
        if (pid && !ul_choom_read(adjust_path, address_of old))
                return ul_bad_usage("choom", "failed to read OOM score adjust value");
        if (!ul_choom_write(adjust_path, (b32)parsed))
                return ul_bad_usage("choom", "failed to set OOM score adjust value");
        if (pid)
        {
                string_format(log,
                              "pid %b's OOM score adjust value changed from %b to %b\n",
                              (bipolar)pid, (bipolar)old, parsed);
                log_flush();
                return 0;
        }
        return ul_exec(taking.first, "choom");
}

// exch ------------------------------------------------------------
#define UL_RENAME_EXCHANGE 2

static const file_long ul_exch_longs[] = {
    {"help", 'h'}, {"version", 'V'}, {null, 0},
};

static b32 util_linux_exch()
{
        file_taking taking = {
            .program = "exch", .allowed = "hV", .longs = ul_exch_longs,
        };
        b32 answer;

        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] OLDPATH NEWPATH",
                    address_of answer))
                return answer;

        positive count = (positive)program_argument_count();
        if (count - taking.first != 2)
                return ul_bad_usage("exch", count - taking.first < 2
                                    ? "too few arguments"
                                    : "too many arguments");

        string_address old = program_argument((b32)taking.first);
        string_address new = program_argument((b32)taking.first + 1);
        bipolar changed = system_rename_at(
            AT_FDCWD, old, AT_FDCWD, new, UL_RENAME_EXCHANGE);
        if (changed < 0)
        {
                string_format(file_fail,
                              "exch: failed to exchange %s and %s: %s\n",
                              old, new, file_reason(changed));
                return 1;
        }
        return 0;
}

// getino ----------------------------------------------------------
static const file_long ul_getino_longs[] = {
    {"pidfs", '0'}, {"cgroupns", '1'}, {"ipcns", '2'},
    {"mntns", '3'}, {"netns", '4'}, {"pidns", '5'},
    {"timens", '6'}, {"userns", '7'}, {"utsns", '8'},
    {"print-pid", 'p'}, {"help", 'h'}, {"version", 'V'}, {null, 0},
};

/* _IO(0xff, n), the pidfs namespace descriptor requests. */
static const p16 ul_getino_requests[] = {
    0, 0xff01, 0xff02, 0xff03, 0xff04, 0xff05, 0xff07, 0xff09, 0xff0a,
};

static b32 util_linux_getino()
{
        file_taking taking = {
            .program = "getino", .allowed = "phV",
            .longs = ul_getino_longs, .operand = file_operand,
        };
        b32 answer;

        file_operands_begin();
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] PID[:inode]...",
                    address_of answer))
                return answer;
        if (file_operand_failed)
                return ul_bad_usage("getino", "not enough memory");
        if (!file_operand_count)
                return ul_bad_usage("getino", "no process specified");

        positive kind = 0;
        bool selected = false;
        for (positive i = 0; i < array_count(ul_getino_requests); i++)
                if (taking.flags & FILE_FLAG((p8)('0' + i)))
                {
                        if (selected)
                                return ul_bad_usage(
                                    "getino", "namespace options are mutually exclusive");
                        selected = true;
                        kind = i;
                }

        bool print_pid = (taking.flags & FILE_FLAG('p')) != 0;
        for (positive i = 0; i < file_operand_count; i++)
        {
                string_address operand = file_operand_at(i);
                b32 pid;
                p64 wanted;

                if (!ul_wait_operand(operand, address_of pid,
                                     address_of wanted))
                {
                        string_format(file_fail,
                                      "getino: invalid PID argument '%s'\n",
                                      operand);
                        return 1;
                }

                bipolar handle = system_call_2(
                    syscall(pidfd_open), (positive)(p32)pid, 0);
                file_facts facts;
                if (handle < 0 ||
                    !file_look(handle, "", AT_EMPTY_PATH, address_of facts) ||
                    (wanted && facts.inode != wanted))
                {
                        if (handle >= 0)
                                system_close(handle);
                        string_format(file_fail,
                                      "getino: could not open PID %b\n",
                                      (bipolar)pid);
                        return 1;
                }

                if (kind)
                {
                        bipolar namespace = system_call_3(
                            syscall(ioctl), (positive)handle,
                            ul_getino_requests[kind], 0);
                        system_close(handle);
                        handle = namespace;
                        if (handle < 0 ||
                            !file_look(handle, "", AT_EMPTY_PATH,
                                       address_of facts))
                        {
                                if (handle >= 0)
                                        system_close(handle);
                                return ul_bad_usage(
                                    "getino", "failed to determine namespace");
                        }
                }

                if (print_pid)
                        string_format(log, "%b:%p\n", (bipolar)pid,
                                      (positive)facts.inode);
                else
                        string_format(log, "%p\n", (positive)facts.inode);
                system_close(handle);
        }

        log_flush();
        return 0;
}

// getopt ----------------------------------------------------------

static const file_long ul_getopt_longs[] = {
    {"alternative", 'a'}, {"longoptions", 'l'},
    {"name", 'n'},        {"options", 'o'},
    {"quiet", 'q'},       {"quiet-output", 'Q'},
    {"shell", 's'},       {"test", 'T'},
    {"unquoted", 'u'},    {"help", 'h'},
    {"version", 'V'},     {null, 0},
};

/* file_taking deliberately keeps only the last value for an option.  getopt
   is the exception: each -l contributes another comma-separated name list.
   Keep views of the original argv strings in the shared text arena; no names
   are copied and no second allocator is involved. */
static string_address address_to ul_getopt_long_lists;
static positive ul_getopt_long_count;
static positive ul_getopt_long_room;

static bool ul_getopt_seen(p8 letter, string_address value)
{
        if (letter != 'l')
                return true;

        if (ul_getopt_long_count >= ul_getopt_long_room)
                return false;

        ul_getopt_long_lists[ul_getopt_long_count++] = value;
        return true;
}

typedef struct
{
        string_address name;
        positive length;
        p8 argument;
        positive matches;
        bool exact;
} ul_getopt_long_match;

/* One walk over one -l value.  util-linux accepts comma and blank separators;
   only the final one or two colons describe the argument, so `name:::` names
   the optional-argument option `name:` just as upstream does. */
static fn ul_getopt_long_each(string_address list,
                              fn(address_to visit)(string_address, positive,
                                                   p8, address_any),
                              address_any context)
{
        string_address at = list;

        while (string_get(at))
        {
                while (string_is(at, ',') || byte_is_space(string_get(at)))
                        at++;
                if (!string_get(at))
                        break;

                string_address first = at;
                while (string_get(at) && !string_is(at, ',') &&
                       !byte_is_space(string_get(at)))
                        at++;

                positive length = (positive)(at - first);
                p8 argument = 0;

                if (length && first[length - 1] == ':')
                {
                        argument = 1;
                        length--;
                        if (length && first[length - 1] == ':')
                        {
                                argument = 2;
                                length--;
                        }
                }

                if (length)
                        visit(first, length, argument, context);
        }
}

typedef struct
{
        string_address wanted;
        positive length;
        ul_getopt_long_match address_to match;
} ul_getopt_long_search;

static fn ul_getopt_long_consider(string_address name, positive length,
                                  p8 argument, address_any opaque)
{
        ul_getopt_long_search address_to search =
            (ul_getopt_long_search address_to)opaque;
        ul_getopt_long_match address_to match = search->match;

        if (length < search->length ||
            string_compare_max(name, search->wanted, search->length))
                return;

        bool exact = length == search->length;

        if (match->exact && !exact)
                return;

        if (exact && !match->exact)
        {
                match->name = name;
                match->length = length;
                match->argument = argument;
                match->matches = 1;
                match->exact = true;
                return;
        }

        /* Repeating the same -l list does not make one spelling ambiguous. */
        if (match->name && match->length == length &&
            !string_compare_max(match->name, name, length))
                return;

        if (!match->name)
        {
                match->name = name;
                match->length = length;
                match->argument = argument;
        }
        match->matches++;
}

static ul_getopt_long_match ul_getopt_long_find(string_address wanted,
                                                 positive length)
{
        ul_getopt_long_match match = {0};
        ul_getopt_long_search search = {wanted, length, address_of match};

        for (positive i = 0; i < ul_getopt_long_count; i++)
                ul_getopt_long_each(ul_getopt_long_lists[i],
                                    ul_getopt_long_consider, address_of search);

        return match;
}

typedef struct
{
        string_address wanted;
        positive length;
        p8 dashes;
        bool first;
} ul_getopt_ambiguity;

static fn ul_getopt_long_possibility(string_address name, positive length,
                                     p8 argument, address_any opaque)
{
        (void)argument;
        ul_getopt_ambiguity address_to ambiguity =
            (ul_getopt_ambiguity address_to)opaque;

        if (length < ambiguity->length ||
            string_compare_max(name, ambiguity->wanted, ambiguity->length))
                return;

        log_error(ambiguity->first ? "; possibilities: '" : " '", 0);
        ambiguity->first = false;
        log_error(ambiguity->dashes == 2 ? "--" : "-", ambiguity->dashes);
        log_error(name, length);
        log_error("'", 1);
}

/* The external getopt protocol has a canonical spelling distinct from the
   shell's human-facing `set` spelling: an embedded quote is '\\'' here.  The
   csh family additionally has to leave blanks and history markers outside a
   quoted run.  This is kept as a protocol writer, not another parser or
   general-purpose quoting layer. */
static fn ul_getopt_quote(string_address value, bool csh)
{
        log("'", 1);

        for (string_address at = value; string_get(at); at++)
        {
                p8 byte = string_get(at);

                if (byte == '\'')
                        log("'\\''", 4);
                else if (csh && (byte == ' ' || byte == '\t' || byte == '!'))
                {
                        log("'\\", 2);
                        log(at, 1);
                        log("'", 1);
                }
                else if (csh && byte == '\n')
                        log("\\n", 2);
                else
                        log(at, 1);
        }

        log("'", 1);
}

static fn ul_getopt_value(string_address value, bool unquoted, bool csh,
                           bool output)
{
        if (!output)
                return;

        log(" ", 1);
        if (unquoted)
                log(value ? value : (string_address)"", 0);
        else
                ul_getopt_quote(value ? value : (string_address)"", csh);
}

static string_address ul_getopt_short_find(string_address options, p8 letter)
{
        if (string_is(options, '+') || string_is(options, '-'))
                options++;
        if (string_is(options, ':'))
                options++;

        return string_first_of(options, letter);
}

static p8 ul_getopt_short_argument(string_address options, p8 letter)
{
        string_address found = ul_getopt_short_find(options, letter);

        if (!found || found[1] != ':')
                return 0;
        return found[2] == ':' ? 2 : 1;
}

static fn ul_getopt_option(p8 letter, string_address argument,
                           p8 argument_kind, bool unquoted, bool csh,
                           bool output)
{
        if (!output)
                return;

        p8 spelling[3] = {' ', '-', letter};
        log(spelling, sizeof(spelling));

        if (argument_kind)
                ul_getopt_value(argument, unquoted, csh, true);
}

static bool ul_getopt_short(string_address word, string_address next,
                            bool has_next, string_address options,
                            string_address name, bool quiet, bool unquoted,
                            bool csh, bool output, bool address_to consumed)
{
        string_address words[4] = {name, word, next, null};
        b32 count = has_next ? 3 : 2;
        b32 saved_optind = optind;
        b32 saved_opterr = opterr;
        string_address saved_optarg = optarg;
        b32 saved_optopt = optopt;
        bool okay = true;

        optind = 0;
        opterr = !quiet;

        while (true)
        {
                b32 letter = getopt(count, words, options);

                if (letter == -1)
                        break;
                if (letter == '?' || letter == ':')
                {
                        okay = false;
                        if (optind >= 2)
                                break;
                        continue;
                }

                p8 kind = ul_getopt_short_argument(options, (p8)letter);
                ul_getopt_option((p8)letter, optarg, kind, unquoted, csh,
                                  output);

                /* A fresh miniature argv presents only this cluster and the
                   one word it may consume as an argument.  Once getopt has
                   advanced past the cluster, do not let the next option word
                   become part of this call; the outer permutation walk owns
                   it. */
                if (optind >= 2)
                        break;
        }

        address_to consumed = optind > 2;
        optind = saved_optind;
        opterr = saved_opterr;
        optarg = saved_optarg;
        optopt = saved_optopt;
        return okay;
}

static bool ul_getopt_long(string_address word, p8 dashes,
                           string_address next, bool has_next,
                           string_address name, bool quiet, bool unquoted,
                           bool csh, bool output, bool address_to consumed)
{
        string_address wanted = word + dashes;
        string_address equal = string_first_of(wanted, '=');
        positive wanted_length = equal ? (positive)(equal - wanted)
                                         : string_length(wanted);
        ul_getopt_long_match match =
            ul_getopt_long_find(wanted, wanted_length);

        address_to consumed = false;

        if (!match.matches)
        {
                if (!quiet)
                        string_format(log_error,
                                      "%s: unrecognized option '%s'\n",
                                      name, word);
                return false;
        }

        if (match.matches > 1 && !match.exact)
        {
                if (!quiet)
                {
                        string_format(log_error, "%s: option '%s' is ambiguous",
                                      name, word);
                        ul_getopt_ambiguity ambiguity = {
                            wanted, wanted_length, dashes, true,
                        };
                        for (positive i = 0; i < ul_getopt_long_count; i++)
                                ul_getopt_long_each(
                                    ul_getopt_long_lists[i],
                                    ul_getopt_long_possibility,
                                    address_of ambiguity);
                        log_error("\n", 1);
                }
                return false;
        }

        string_address argument = equal ? equal + 1 : null;

        if (!match.argument && equal)
        {
                if (!quiet)
                {
                        string_format(log_error, "%s: option '", name);
                        log_error(dashes == 2 ? "--" : "-", dashes);
                        log_error(match.name, match.length);
                        log_error("' doesn't allow an argument\n", 0);
                }
                return false;
        }

        if (match.argument == 1 && !equal)
        {
                if (!has_next)
                {
                        if (!quiet)
                        {
                                string_format(log_error, "%s: option '", name);
                                log_error(dashes == 2 ? "--" : "-", dashes);
                                log_error(match.name, match.length);
                                log_error("' requires an argument\n", 0);
                        }
                        return false;
                }
                argument = next;
                address_to consumed = true;
        }

        if (output)
        {
                log(" --", 3);
                log(match.name, match.length);
        }
        if (match.argument)
                ul_getopt_value(argument, unquoted, csh, output);
        return true;
}

static COLD b32 ul_getopt_setup_error(string_address message)
{
        string_format(log_error, "getopt: %s\n"
                      "Try 'getopt --help' for more information.\n", message);
        return 2;
}

static b32 util_linux_getopt()
{
        positive count = (positive)program_argument_count();
        bool compatible = count > 1 &&
            !string_is(program_argument(1), '-');
        string_address options;
        positive first;
        bool alternative = false;
        bool quiet = false;
        bool quiet_output = false;
        bool unquoted = compatible;
        bool csh = false;
        string_address diagnostic_name = "getopt";

        text_arena_used = 0;
        ul_getopt_long_count = 0;
        ul_getopt_long_room = count;
        ul_getopt_long_lists = count
            ? (string_address address_to)text_arena_take(
                  count * sizeof(*ul_getopt_long_lists))
            : null;
        if (count && !ul_getopt_long_lists)
                return 2;

        if (compatible)
        {
                options = program_argument(1);
                first = 2;
        }
        else
        {
                file_taking taking = {
                    .program = "getopt", .allowed = "alnoqQsTuhV",
                    .valued = "lnos", .longs = ul_getopt_longs,
                    .seen = ul_getopt_seen,
                };
                b32 answer;

                if (!file_take(address_of taking))
                        return 2;
                if (taking.flags & FILE_FLAG('T'))
                        return 4;
                if (ul_meta(address_of taking,
                            "[options] optstring parameters", address_of answer))
                        return answer;

                if (taking.flags & FILE_FLAG('o'))
                {
                        options = file_option_value(address_of taking, 'o');
                        first = taking.first;
                }
                else
                {
                        if (taking.first >= count)
                                return ul_getopt_setup_error(
                                    "missing optstring argument");
                        options = program_argument((b32)taking.first);
                        first = taking.first + 1;
                }

                alternative = (taking.flags & FILE_FLAG('a')) != 0;
                quiet = (taking.flags & FILE_FLAG('q')) != 0;
                quiet_output = (taking.flags & FILE_FLAG('Q')) != 0;
                unquoted = (taking.flags & FILE_FLAG('u')) != 0;
                if (file_option_value(address_of taking, 'n'))
                        diagnostic_name =
                            file_option_value(address_of taking, 'n');

                string_address shell = file_option_value(address_of taking, 's');
                if (shell)
                {
                        if (string_equals(shell, "csh") ||
                            string_equals(shell, "tcsh"))
                                csh = true;
                        else if (!string_equals(shell, "sh") &&
                                 !string_equals(shell, "bash"))
                                return ul_getopt_setup_error(
                                    "unknown shell after -s or --shell argument");
                }
        }

        bool address_to deferred = count
            ? (bool address_to)text_arena_take(count * sizeof(*deferred))
            : null;
        if (count && !deferred)
                return 2;
        memory_fill(deferred, 0, count * sizeof(*deferred));

        bool failed = false;
        bool stopped = false;
        bool positive_order = string_is(options, '+');
        bool return_in_order = string_is(options, '-');
        bool output = !quiet_output;
        string_address option_body = options;
        if (string_is(option_body, '+') || string_is(option_body, '-'))
                option_body++;
        bool target_quiet = quiet || string_is(option_body, ':');

        for (positive i = first; i < count; i++)
        {
                string_address word = program_argument((b32)i);

                if (stopped)
                {
                        deferred[i] = true;
                        continue;
                }
                if (string_equals(word, "--"))
                {
                        stopped = true;
                        continue;
                }
                if (!string_is(word, '-') || !string_get(word + 1))
                {
                        if (positive_order)
                                stopped = true;
                        if (return_in_order)
                                ul_getopt_value(word, unquoted, csh, output);
                        else
                                deferred[i] = true;
                        continue;
                }

                bool double_dash = string_is(word + 1, '-');
                bool long_word = double_dash && string_get(word + 2);
                p8 dashes = double_dash ? 2 : 1;
                ul_getopt_long_match match = {0};

                if (!double_dash && alternative)
                {
                        string_address wanted = word + 1;
                        string_address equal = string_first_of(wanted, '=');
                        positive length = equal ? (positive)(equal - wanted)
                                                : string_length(wanted);
                        match = ul_getopt_long_find(wanted, length);

                        /* getopt_long_only gives a recognized long spelling
                           priority; when there is none, a valid first short
                           letter makes this an ordinary option cluster. */
                        bool short_first = ul_getopt_short_find(
                            options, string_get(word + 1)) != null;
                        bool one_letter = !string_get(word + 2);

                        long_word =
                            (match.matches && !(one_letter && short_first)) ||
                            (!match.matches && !short_first);
                }

                bool consumed = false;
                bool okay;
                string_address next = i + 1 < count
                    ? program_argument((b32)i + 1) : null;

                if (long_word)
                        /* A leading ':' silences long diagnostics as well as
                           the shared short parser. */
                        okay = ul_getopt_long(word, dashes, next, i + 1 < count,
                                              diagnostic_name, target_quiet,
                                              unquoted, csh,
                                              output, address_of consumed);
                else
                        okay = ul_getopt_short(word, next, i + 1 < count,
                                               options, diagnostic_name,
                                               target_quiet,
                                               unquoted, csh, output,
                                               address_of consumed);

                if (!okay)
                        failed = true;
                if (consumed)
                        i++;
        }

        if (output)
        {
                log(" --", 3);
                for (positive i = first; i < count; i++)
                        if (deferred[i])
                                ul_getopt_value(program_argument((b32)i),
                                                unquoted, csh, true);
                log("\n", 1);
                log_flush();
        }

        return failed ? 1 : 0;
}

// lscpu ------------------------------------------------------------

/* Linux has already normalized CPU topology into sysfs, and /proc/cpuinfo
   has the descriptive strings.  Reading those exports avoids a second CPUID
   implementation beside moonwater_cpu_detect; every output mode below is a
   view of this one snapshot and reuses the common CPU-list and table engines. */
#define UL_LSCPU_CACHE_MAX 8

typedef struct
{
        positive id;
        bipolar core;
        bipolar socket;
        bipolar cluster;
        bipolar node;
        positive current_khz;
        positive maximum_khz;
        positive minimum_khz;
        b32 cache[UL_LSCPU_CACHE_MAX];
        bool online;
} ul_lscpu_cpu;

typedef struct
{
        p8 name[8];
        p8 type[16];
        positive level;
        positive size;
        positive ways;
        positive sets;
        positive physical_line;
        positive coherency;
        positive instances;
} ul_lscpu_cache;

typedef struct
{
        p8 kind;
        positive first_cpu;
        b32 id;
} ul_lscpu_instance;

typedef struct
{
        string_address vendor;
        string_address model_name;
        string_address family;
        string_address model;
        string_address stepping;
        string_address microcode;
        string_address bogomips;
        string_address flags;
        string_address implementer;
} ul_lscpu_info;

typedef struct
{
        ul_lscpu_cpu address_to cpus;
        ul_lscpu_instance address_to instances;
        positive cpu_count;
        positive present_count;
        positive online_count;
        positive core_count;
        positive socket_count;
        positive cluster_count;
        positive node_count;
        positive instance_count;
        positive cache_count;
        ul_lscpu_cache caches[UL_LSCPU_CACHE_MAX];
        ul_lscpu_info info;
        file_machine machine;
} ul_lscpu_snapshot;

static ul_lscpu_snapshot ul_lscpu;
static positive ul_lscpu_present[UL_CPU_WORDS];
static positive ul_lscpu_online[UL_CPU_WORDS];
static positive ul_lscpu_scratch_set[UL_CPU_WORDS];
static p8 ul_lscpu_cpuinfo[64 << 10];
static p8 ul_lscpu_cache_heading[64];

static positive ul_lscpu_set_count(positive address_to set)
{
        positive count = 0;
        for (positive i = 0; i < UL_CPU_WORDS; i++)
                count += bits_counted(set[i]);
        return count;
}

static bool ul_lscpu_set_read(string_address path, positive address_to set)
{
        p8 text[FILE_PATH_MAX];
        return ul_slurp_word(path, text, sizeof(text)) > 0 &&
               ul_cpu_list(text, set);
}

static string_address ul_lscpu_keep(string_address text)
{
        positive length = string_length(text);
        p8 address_to copy = text_arena_take(length + 1);
        if (!copy)
                return (string_address)"";
        memory_copy(copy, text, length + 1);
        return copy;
}

static string_address ul_lscpu_number(positive value)
{
        p8 text[24];
        positive length = positive_into_string(text, value);
        text[length] = end;
        return ul_lscpu_keep(text);
}

static fn ul_lscpu_cpu_path(p8 address_to path, positive cpu,
                            string_address property)
{
        string_address base =
            (string_address)"/sys/devices/system/cpu/cpu";
        positive at = string_length(base);
        memory_copy(path, base, at);
        at += positive_into_string(path + at, cpu);
        path[at++] = '/';
        positive length = string_length(property);
        memory_copy(path + at, property, length + 1);
}

static fn ul_lscpu_cache_path(p8 address_to path, positive cpu,
                              positive index, string_address property)
{
        ul_lscpu_cpu_path(path, cpu, (string_address)"cache/index");
        positive at = string_length(path);
        at += positive_into_string(path + at, index);
        path[at++] = '/';
        positive length = string_length(property);
        memory_copy(path + at, property, length + 1);
}

static bool ul_lscpu_file_number(string_address path,
                                 positive address_to value)
{
        p8 text[64];
        return ul_slurp_word(path, text, sizeof(text)) > 0 &&
               ul_unsigned(text, positive_max, value);
}

static bool ul_lscpu_cpu_number(positive cpu, string_address property,
                                positive address_to value)
{
        p8 path[192];
        ul_lscpu_cpu_path(path, cpu, property);
        return ul_lscpu_file_number(path, value);
}

static fn ul_lscpu_info_read()
{
        ul_lscpu_info address_to info = address_of ul_lscpu.info;
        memory_fill(info, 0, sizeof(*info));
        bipolar got = file_slurp((string_address)"/proc/cpuinfo",
                                 ul_lscpu_cpuinfo,
                                 sizeof(ul_lscpu_cpuinfo));
        if (got <= 0)
                return;

        positive at = 0;
        while (at < (positive)got)
        {
                positive start = at;
                while (at < (positive)got && ul_lscpu_cpuinfo[at] != '\n')
                        at++;
                positive finish = at;
                if (at < (positive)got)
                        ul_lscpu_cpuinfo[at++] = end;
                while (finish > start &&
                       byte_is_space(ul_lscpu_cpuinfo[finish - 1]))
                        ul_lscpu_cpuinfo[--finish] = end;
                if (finish == start)
                        break;

                p8 address_to colon = memory_first_of(
                    ul_lscpu_cpuinfo + start, ':', finish - start);
                if (!colon)
                        continue;
                positive key_length =
                    (positive)(colon - (ul_lscpu_cpuinfo + start));
                while (key_length &&
                       byte_is_space(ul_lscpu_cpuinfo[start + key_length - 1]))
                        key_length--;
                p8 address_to value = colon + 1;
                while (byte_is_space(*value))
                        value++;
                p8 address_to key = ul_lscpu_cpuinfo + start;

#define UL_LSCPU_INFO(member, spelling)                                     \
                if (!info->member &&                                        \
                    file_same_word(key, key_length,                          \
                                   (string_address)spelling))                \
                        info->member = value
                UL_LSCPU_INFO(vendor, "vendor_id");
                UL_LSCPU_INFO(model_name, "model name");
                UL_LSCPU_INFO(family, "cpu family");
                UL_LSCPU_INFO(model, "model");
                UL_LSCPU_INFO(stepping, "stepping");
                UL_LSCPU_INFO(microcode, "microcode");
                UL_LSCPU_INFO(bogomips, "bogomips");
                UL_LSCPU_INFO(flags, "flags");
                UL_LSCPU_INFO(flags, "features");
                UL_LSCPU_INFO(implementer, "cpu implementer");
                UL_LSCPU_INFO(model, "cpu part");
                UL_LSCPU_INFO(stepping, "cpu variant");
#undef UL_LSCPU_INFO
        }

        if (!info->vendor && info->implementer)
        {
                static const struct
                {
                        string_address id;
                        string_address name;
                } vendors[] = {
                    {"0x41", "ARM"}, {"0x42", "Broadcom"},
                    {"0x43", "Cavium"}, {"0x46", "Fujitsu"},
                    {"0x4e", "NVIDIA"}, {"0x51", "Qualcomm"},
                    {"0x53", "Samsung"}, {"0x56", "Marvell"},
                    {"0x61", "Apple"}, {"0x69", "Intel"},
                };
                for (positive i = 0; i < array_count(vendors); i++)
                        if (file_same_word(info->implementer,
                                           string_length(info->implementer),
                                           vendors[i].id))
                                info->vendor = vendors[i].name;
        }

        if (!info->flags)
        {
#if X64 || X86
                info->flags = cpu_has_avx512 ? (string_address)"avx2 avx512f"
                              : cpu_has_avx2 ? (string_address)"avx2"
                                             : (string_address)"";
#else
                info->flags = (string_address)"";
#endif
        }

#if ARM64
        if (info->model && string_is(info->model, '0') &&
            byte_to_lower(info->model[1]) == 'x')
        {
                string_address at = info->model + 2;
                positive number;
                if (string_digits_checked(address_of at, 16,
                                           address_of number) && !string_get(at))
                        info->model = ul_lscpu_number(number);
        }
#endif
}

static b32 ul_lscpu_cache_kind(positive level, string_address type)
{
        p8 name[8] = {'L'};
        positive length = 1 + positive_into_string(name + 1, level);
        p8 kind = byte_to_lower(string_get(type));
        if (kind == 'd' || kind == 'i')
                name[length++] = kind;
        name[length] = end;

        for (positive i = 0; i < ul_lscpu.cache_count; i++)
                if (string_equals(name, ul_lscpu.caches[i].name))
                        return (b32)i;
        if (ul_lscpu.cache_count == UL_LSCPU_CACHE_MAX)
                return -1;

        positive i = ul_lscpu.cache_count++;
        string_copy_max_end(ul_lscpu.caches[i].name, name,
                            sizeof(ul_lscpu.caches[i].name) - 1);
        string_copy_max_end(ul_lscpu.caches[i].type, type,
                            sizeof(ul_lscpu.caches[i].type) - 1);
        ul_lscpu.caches[i].level = level;
        return (b32)i;
}

static b32 ul_lscpu_cache_instance(p8 kind, positive first_cpu)
{
        for (positive i = 0; i < ul_lscpu.instance_count; i++)
                if (ul_lscpu.instances[i].kind == kind &&
                    ul_lscpu.instances[i].first_cpu == first_cpu)
                        return ul_lscpu.instances[i].id;

        ul_lscpu_instance address_to item =
            ul_lscpu.instances + ul_lscpu.instance_count++;
        item->kind = kind;
        item->first_cpu = first_cpu;
        item->id = (b32)ul_lscpu.caches[kind].instances++;
        return item->id;
}

static fn ul_lscpu_cache_read(ul_lscpu_cpu address_to cpu)
{
        for (positive index = 0; index < UL_LSCPU_CACHE_MAX; index++)
        {
                p8 path[192];
                p8 type[32];
                positive level;
                ul_lscpu_cache_path(path, cpu->id, index, "level");
                if (!ul_lscpu_file_number(path, address_of level))
                        continue;
                ul_lscpu_cache_path(path, cpu->id, index, "type");
                if (ul_slurp_word(path, type, sizeof(type)) <= 0)
                        continue;
                b32 kind = ul_lscpu_cache_kind(level, type);
                if (kind < 0)
                        continue;

                positive first = cpu->id;
                p8 shared[FILE_PATH_MAX];
                ul_lscpu_cache_path(path, cpu->id, index, "shared_cpu_list");
                if (ul_slurp_word(path, shared, sizeof(shared)) > 0 &&
                    ul_cpu_list(shared, ul_lscpu_scratch_set))
                        for (positive i = 0; i < UL_CPU_BITS; i++)
                                if (ul_cpu_has(ul_lscpu_scratch_set, i))
                                {
                                        first = i;
                                        break;
                                }
                cpu->cache[kind] =
                    ul_lscpu_cache_instance((p8)kind, first);

                ul_lscpu_cache address_to cache = ul_lscpu.caches + kind;
                if (cache->size)
                        continue;
                p8 size[64];
                ul_lscpu_cache_path(path, cpu->id, index, "size");
                if (ul_slurp_word(path, size, sizeof(size)) > 0)
                        (void)split_size(size, address_of cache->size);
#define UL_LSCPU_CACHE_NUMBER(member, property)                              \
                ul_lscpu_cache_path(path, cpu->id, index, property);         \
                (void)ul_lscpu_file_number(path, address_of cache->member)
                UL_LSCPU_CACHE_NUMBER(ways, "ways_of_associativity");
                UL_LSCPU_CACHE_NUMBER(sets, "number_of_sets");
                UL_LSCPU_CACHE_NUMBER(physical_line,
                                      "physical_line_partition");
                UL_LSCPU_CACHE_NUMBER(coherency, "coherency_line_size");
#undef UL_LSCPU_CACHE_NUMBER
        }
}

static fn ul_lscpu_numa_read()
{
        file_walk walk;
        if (file_walk_open(address_of walk, AT_FDCWD,
                           (string_address)"/sys/devices/system/node"))
        {
                struct linux_dirent64 address_to entry;
                while ((entry = file_walk_next(address_of walk)))
                {
                        string_address name = (string_address)entry->d_name;
                        if (string_compare_max(name, "node", 4))
                                continue;
                        positive node;
                        if (!ul_unsigned(name + 4, positive_max,
                                         address_of node))
                                continue;
                        p8 directory[96];
                        p8 path[112];
                        path_join(directory, sizeof(directory),
                                  (string_address)"/sys/devices/system/node",
                                  name);
                        path_join(path, sizeof(path), directory,
                                  (string_address)"cpulist");
                        if (!ul_lscpu_set_read(path, ul_lscpu_scratch_set))
                                continue;
                        ul_lscpu.node_count++;
                        for (positive i = 0; i < ul_lscpu.cpu_count; i++)
                                if (ul_cpu_has(ul_lscpu_scratch_set,
                                               ul_lscpu.cpus[i].id))
                                        ul_lscpu.cpus[i].node = (bipolar)node;
                }
                file_walk_close(address_of walk);
        }

        if (!ul_lscpu.node_count)
        {
                ul_lscpu.node_count = 1;
                for (positive i = 0; i < ul_lscpu.cpu_count; i++)
                        ul_lscpu.cpus[i].node = 0;
        }
}

static bool ul_lscpu_take()
{
        memory_fill(address_of ul_lscpu, 0, sizeof(ul_lscpu));
        memory_fill(ul_lscpu_present, 0, sizeof(ul_lscpu_present));
        memory_fill(ul_lscpu_online, 0, sizeof(ul_lscpu_online));

        bool present = ul_lscpu_set_read(
            (string_address)"/sys/devices/system/cpu/present",
            ul_lscpu_present);
        if (!present)
                present = ul_lscpu_set_read(
                    (string_address)"/sys/devices/system/cpu/possible",
                    ul_lscpu_present);
        bool online = ul_lscpu_set_read(
            (string_address)"/sys/devices/system/cpu/online",
            ul_lscpu_online);

        if (!present)
        {
                if (!nproc_affinity_count())
                        return false;
                memory_copy(ul_lscpu_present, nproc_affinity_words,
                            sizeof(ul_lscpu_present));
        }
        if (!online)
                memory_copy(ul_lscpu_online, ul_lscpu_present,
                            sizeof(ul_lscpu_online));

        ul_lscpu.present_count = ul_lscpu_set_count(ul_lscpu_present);
        ul_lscpu.online_count = ul_lscpu_set_count(ul_lscpu_online);
        if (!ul_lscpu.present_count ||
            ul_lscpu.present_count >
                TEXT_ARENA_BYTES / sizeof(ul_lscpu_cpu))
                return false;
        ul_lscpu.cpus = text_arena_take(
            ul_lscpu.present_count * sizeof(ul_lscpu_cpu));
        positive maximum_instances =
            ul_lscpu.present_count * UL_LSCPU_CACHE_MAX;
        if (!ul_lscpu.cpus ||
            maximum_instances > (TEXT_ARENA_BYTES - text_arena_used) /
                                    sizeof(ul_lscpu_instance))
                return false;
        ul_lscpu.instances = text_arena_take(
            maximum_instances * sizeof(ul_lscpu_instance));
        if (!ul_lscpu.instances)
                return false;

        ul_lscpu_info_read();
        memory_fill(address_of ul_lscpu.machine, 0,
                    sizeof(ul_lscpu.machine));
        (void)system_call_1(syscall(uname),
                            (positive)address_of ul_lscpu.machine);

        for (positive id = 0; id < UL_CPU_BITS; id++)
        {
                if (!ul_cpu_has(ul_lscpu_present, id))
                        continue;
                ul_lscpu_cpu address_to cpu =
                    ul_lscpu.cpus + ul_lscpu.cpu_count++;
                memory_fill(cpu, 0, sizeof(*cpu));
                cpu->id = id;
                cpu->core = cpu->socket = cpu->cluster = cpu->node = -1;
                for (positive c = 0; c < UL_LSCPU_CACHE_MAX; c++)
                        cpu->cache[c] = -1;
                cpu->online = ul_cpu_has(ul_lscpu_online, id);

                positive value;
                if (ul_lscpu_cpu_number(id, "topology/core_id",
                                        address_of value))
                        cpu->core = (bipolar)value;
                if (ul_lscpu_cpu_number(id, "topology/physical_package_id",
                                        address_of value))
                        cpu->socket = (bipolar)value;
                if (ul_lscpu_cpu_number(id, "topology/cluster_id",
                                        address_of value) && value != 65535)
                        cpu->cluster = (bipolar)value;
#if ARM64
                if (cpu->cluster < 0 && cpu->socket >= 0)
                {
                        cpu->cluster = cpu->socket;
                        cpu->socket = -1;
                }
#endif
                if (ul_lscpu_cpu_number(id, "cpufreq/scaling_cur_freq",
                                        address_of value))
                        cpu->current_khz = value;
                if (ul_lscpu_cpu_number(id, "cpufreq/cpuinfo_max_freq",
                                        address_of value))
                        cpu->maximum_khz = value;
                if (ul_lscpu_cpu_number(id, "cpufreq/cpuinfo_min_freq",
                                        address_of value))
                        cpu->minimum_khz = value;
                ul_lscpu_cache_read(cpu);
        }

        ul_lscpu_numa_read();

        positive cache_heading = 0;
        for (positive i = 0; i < ul_lscpu.cache_count; i++)
        {
                if (i)
                        ul_lscpu_cache_heading[cache_heading++] = ':';
                positive length = string_length(ul_lscpu.caches[i].name);
                memory_copy(ul_lscpu_cache_heading + cache_heading,
                            ul_lscpu.caches[i].name, length);
                cache_heading += length;
        }
        ul_lscpu_cache_heading[cache_heading] = end;

        for (positive i = 0; i < ul_lscpu.cpu_count; i++)
        {
                ul_lscpu_cpu address_to cpu = ul_lscpu.cpus + i;
                bool core_first = true;
                bool socket_first = cpu->socket >= 0;
                bool cluster_first = cpu->cluster >= 0;
                for (positive j = 0; j < i; j++)
                {
                        ul_lscpu_cpu address_to old = ul_lscpu.cpus + j;
                        if (old->core == cpu->core &&
                            old->socket == cpu->socket)
                                core_first = false;
                        if (old->socket == cpu->socket)
                                socket_first = false;
                        if (old->cluster == cpu->cluster)
                                cluster_first = false;
                }
                if (core_first)
                        ul_lscpu.core_count++;
                if (socket_first)
                        ul_lscpu.socket_count++;
                if (cluster_first)
                        ul_lscpu.cluster_count++;
        }
        return true;
}

static positive ul_lscpu_set_text(p8 address_to text, positive room,
                                  positive address_to set, bool hex)
{
        positive used = 0;
        if (hex)
        {
                positive nibbles = UL_CPU_BITS / 4;
                while (nibbles > 1)
                {
                        positive n = nibbles - 1;
                        if ((set[n / (positive_bits / 4)] >>
                             ((n % (positive_bits / 4)) * 4)) & 15)
                                break;
                        nibbles--;
                }
                for (positive left = nibbles; left && used + 1 < room; left--)
                {
                        positive n = left - 1;
                        positive digit = set[n / (positive_bits / 4)] >>
                            ((n % (positive_bits / 4)) * 4) & 15;
                        if (left != nibbles && !(left % 8))
                                text[used++] = ',';
                        text[used++] = storage_hex_digit((p8)digit, false);
                }
        }
        else
        {
                bool comma = false;
                for (positive first = 0; first < UL_CPU_BITS; first++)
                {
                        if (!ul_cpu_has(set, first))
                                continue;
                        positive last = first;
                        while (last + 1 < UL_CPU_BITS &&
                               ul_cpu_has(set, last + 1))
                                last++;
                        if (comma)
                                text[used++] = ',';
                        used += positive_into_string(text + used, first);
                        if (last != first)
                        {
                                text[used++] = '-';
                                used += positive_into_string(text + used, last);
                        }
                        if (used + 24 >= room)
                                break;
                        comma = true;
                        first = last;
                }
        }
        text[min(used, room - 1)] = end;
        return used;
}

typedef struct
{
        string_address field;
        string_address data;
} ul_lscpu_summary_item;

static string_address ul_lscpu_summary_field(address_any row, p8 column,
                                              p8 address_to scratch)
{
        (void)scratch;
        ul_lscpu_summary_item address_to item =
            (ul_lscpu_summary_item address_to)row;
        return column ? item->data : item->field;
}

static const ul_table_column ul_lscpu_summary_columns[] = {
    {"field", "FIELD", 0, false, UL_TABLE_STRING},
    {"data", "DATA", 0, false, UL_TABLE_STRING},
};

static fn ul_lscpu_summary_add(ul_lscpu_summary_item address_to items,
                               positive address_to count,
                               string_address field, string_address data)
{
        if (data && string_get(data))
                items[(address_to count)++] =
                    (ul_lscpu_summary_item){field, data};
}

static string_address ul_lscpu_cache_size(positive size, bool bytes,
                                          bool spaced)
{
        p8 text[48];
        positive used;
        if (bytes)
                used = positive_into_string(text, size);
        else
        {
                positive unit = 1;
                p8 suffix = 'B';
                static const p8 suffixes[] = "KMGTPE";
                for (positive power = 0;
                     power + 1 < sizeof(suffixes) &&
                     unit <= positive_max / 1024 && size >= unit * 1024;
                     power++)
                {
                        unit *= 1024;
                        suffix = suffixes[power];
                }

                positive whole = size / unit;
                positive remainder = size % unit;
                positive tenth = 0;
                positive fraction = 0;
                /* Compute remainder * 10 / unit without widening division:
                   every step adds modulo unit with an overflow-free compare.
                   The final remainder decides round-to-nearest. */
                for (positive i = 0; remainder && i < 10; i++)
                        if (fraction >= unit - remainder)
                        {
                                fraction -= unit - remainder;
                                tenth++;
                        }
                        else
                                fraction += remainder;
                if (fraction >= (unit + 1) / 2)
                        tenth++;
                if (tenth == 10)
                {
                        whole++;
                        tenth = 0;
                }
                used = positive_into_string(text, whole);
                if (tenth)
                {
                        text[used++] = '.';
                        text[used++] = (p8)('0' + tenth);
                }
                if (spaced)
                        text[used++] = ' ';
                text[used++] = suffix;
                if (spaced && suffix != 'B')
                {
                        text[used++] = 'i';
                        text[used++] = 'B';
                }
        }
        text[used] = end;
        return ul_lscpu_keep(text);
}

static string_address ul_lscpu_cache_summary(ul_lscpu_cache address_to cache,
                                             bool bytes)
{
        positive total = cache->size;
        if (cache->instances && total <= positive_max / cache->instances)
                total *= cache->instances;
        string_address size = ul_lscpu_cache_size(total, bytes, true);
        if (cache->instances <= 1)
                return size;

        p8 text[80];
        positive used = string_length(size);
        memory_copy(text, size, used);
        memory_copy(text + used, " (", 2);
        used += 2;
        used += positive_into_string(text + used, cache->instances);
        memory_copy(text + used, " instances)", 11);
        used += 11;
        text[used] = end;
        return ul_lscpu_keep(text);
}

static fn ul_lscpu_summary(bool json, bool hex, bool bytes)
{
        ul_lscpu_summary_item address_to items = text_arena_take(
            96 * sizeof(ul_lscpu_summary_item));
        if (!items)
                return;
        positive count = 0;
        ul_lscpu_info address_to info = address_of ul_lscpu.info;

        ul_lscpu_summary_add(items, address_of count, "Architecture:",
                             ul_lscpu.machine.machine);
        ul_lscpu_summary_add(items, address_of count, "CPU op-mode(s):",
#if X64 || X86
                             "32-bit, 64-bit"
#else
                             "64-bit"
#endif
        );
        ul_lscpu_summary_add(items, address_of count, "Byte Order:",
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                             "Little Endian"
#else
                             "Big Endian"
#endif
        );
        ul_lscpu_summary_add(items, address_of count, "CPU(s):",
                             ul_lscpu_number(ul_lscpu.present_count));

        p8 set[FILE_PATH_MAX];
        ul_lscpu_set_text(set, sizeof(set), ul_lscpu_online, hex);
        ul_lscpu_summary_add(items, address_of count,
                             "On-line CPU(s) list:", ul_lscpu_keep(set));
        for (positive i = 0; i < UL_CPU_WORDS; i++)
                ul_lscpu_scratch_set[i] =
                    ul_lscpu_present[i] & ~ul_lscpu_online[i];
        if (ul_lscpu_set_count(ul_lscpu_scratch_set))
        {
                ul_lscpu_set_text(set, sizeof(set), ul_lscpu_scratch_set, hex);
                ul_lscpu_summary_add(items, address_of count,
                                     "Off-line CPU(s) list:",
                                     ul_lscpu_keep(set));
        }

        ul_lscpu_summary_add(items, address_of count, "Vendor ID:",
                             info->vendor ? info->vendor
                                          : (string_address)"-");
        ul_lscpu_summary_add(items, address_of count, "Model name:",
                             info->model_name ? info->model_name
                                              : (string_address)"-");
        ul_lscpu_summary_add(items, address_of count, "CPU family:",
                             info->family);
        ul_lscpu_summary_add(items, address_of count, "Model:", info->model);
        positive threads = ul_lscpu.core_count
                               ? ul_lscpu.present_count / ul_lscpu.core_count
                               : 1;
        ul_lscpu_summary_add(items, address_of count, "Thread(s) per core:",
                             ul_lscpu_number(max(threads, (positive)1)));
        if (ul_lscpu.socket_count)
        {
                ul_lscpu_summary_add(
                    items, address_of count, "Core(s) per socket:",
                    ul_lscpu_number(ul_lscpu.core_count /
                                    ul_lscpu.socket_count));
                ul_lscpu_summary_add(items, address_of count, "Socket(s):",
                                     ul_lscpu_number(ul_lscpu.socket_count));
        }
        else
                ul_lscpu_summary_add(items, address_of count, "Socket(s):",
                                     "-");
        if (ul_lscpu.cluster_count)
        {
                ul_lscpu_summary_add(
                    items, address_of count, "Core(s) per cluster:",
                    ul_lscpu_number(ul_lscpu.core_count /
                                    ul_lscpu.cluster_count));
                ul_lscpu_summary_add(items, address_of count, "Cluster(s):",
                                     ul_lscpu_number(ul_lscpu.cluster_count));
        }
        ul_lscpu_summary_add(items, address_of count, "Stepping:",
                             info->stepping);
        ul_lscpu_summary_add(items, address_of count, "Microcode version:",
                             info->microcode);
        ul_lscpu_summary_add(items, address_of count, "BogoMIPS:",
                             info->bogomips);
        ul_lscpu_summary_add(items, address_of count, "Flags:", info->flags);

        if (info->flags &&
            (string_search(info->flags, " svm") ||
             !string_compare_max(info->flags, "svm ", 4)))
                ul_lscpu_summary_add(items, address_of count, "Virtualization:",
                                     "AMD-V");
        else if (info->flags &&
                 (string_search(info->flags, " vmx") ||
                  !string_compare_max(info->flags, "vmx ", 4)))
                ul_lscpu_summary_add(items, address_of count, "Virtualization:",
                                     "VT-x");

        for (positive i = 0; i < ul_lscpu.cache_count; i++)
        {
                ul_lscpu_cache address_to cache = ul_lscpu.caches + i;
                if (!cache->size)
                        continue;
                p8 label[24];
                positive used = string_length(cache->name);
                memory_copy(label, cache->name, used);
                memory_copy(label + used, " cache:", 8);
                label[used + 7] = end;
                ul_lscpu_summary_add(items, address_of count,
                                     ul_lscpu_keep(label),
                                     ul_lscpu_cache_summary(cache, bytes));
        }

        ul_lscpu_summary_add(items, address_of count, "NUMA node(s):",
                             ul_lscpu_number(ul_lscpu.node_count));
        for (positive i = 0; i < ul_lscpu.cpu_count; i++)
        {
                bipolar node = ul_lscpu.cpus[i].node;
                bool first = node >= 0;
                for (positive j = 0; j < i; j++)
                        if (ul_lscpu.cpus[j].node == node)
                                first = false;
                if (!first)
                        continue;
                memory_fill(ul_lscpu_scratch_set, 0,
                            sizeof(ul_lscpu_scratch_set));
                for (positive j = 0; j < ul_lscpu.cpu_count; j++)
                        if (ul_lscpu.cpus[j].node == node)
                        {
                                positive cpu = ul_lscpu.cpus[j].id;
                                ul_lscpu_scratch_set[cpu / positive_bits] |=
                                    (positive)1 << (cpu % positive_bits);
                        }
                ul_lscpu_set_text(set, sizeof(set), ul_lscpu_scratch_set, hex);
                p8 label[48];
                positive used = memory_copy_end(label, "NUMA node", 9) - label;
                used += positive_into_string(label + used, (positive)node);
                memory_copy(label + used, " CPU(s):", 9);
                label[used + 8] = end;
                ul_lscpu_summary_add(items, address_of count,
                                     ul_lscpu_keep(label), ul_lscpu_keep(set));
        }

        file_walk walk;
        if (file_walk_open(address_of walk, AT_FDCWD,
                           "/sys/devices/system/cpu/vulnerabilities"))
        {
                struct linux_dirent64 address_to entry;
                while (count < 96 && (entry = file_walk_next(address_of walk)))
                {
                        string_address name = entry->d_name;
                        if (string_is(name, '.'))
                                continue;
                        p8 path[160];
                        p8 value[512];
                        path_join(path, sizeof(path),
                                  "/sys/devices/system/cpu/vulnerabilities",
                                  name);
                        if (ul_slurp_word(path, value, sizeof(value)) <= 0)
                                continue;
                        p8 label[128];
                        positive used = memory_copy_end(
                            label, "Vulnerability ", 14) - label;
                        for (positive j = 0; string_get(name + j) &&
                                             used + 2 < sizeof(label); j++)
                        {
                                p8 byte = name[j] == '_' ? ' ' : name[j];
                                label[used++] = j ? byte : byte_to_upper(byte);
                        }
                        label[used++] = ':';
                        label[used] = end;
                        ul_lscpu_summary_add(items, address_of count,
                                             ul_lscpu_keep(label),
                                             ul_lscpu_keep(value));
                }
                file_walk_close(address_of walk);
        }

        p8 columns[] = {0, 1};
        if (json)
                ul_table_json("lscpu", items, sizeof(*items), count,
                              ul_lscpu_summary_columns, columns, 2,
                              ul_lscpu_summary_field);
        else
                ul_table_out(items, sizeof(*items), count,
                             ul_lscpu_summary_columns, 2, columns, 2,
                             false, false, ul_lscpu_summary_field);
}

enum
{
        UL_LSCPU_CPU,
        UL_LSCPU_CORE,
        UL_LSCPU_SOCKET,
        UL_LSCPU_CLUSTER,
        UL_LSCPU_NODE,
        UL_LSCPU_CACHE,
        UL_LSCPU_ONLINE,
        UL_LSCPU_BOGOMIPS,
        UL_LSCPU_MICROCODE,
        UL_LSCPU_MHZ,
        UL_LSCPU_SCALMHZ,
        UL_LSCPU_MAXMHZ,
        UL_LSCPU_MINMHZ,
        UL_LSCPU_MODELNAME,
        UL_LSCPU_COLUMNS,
};

static ul_table_column ul_lscpu_columns[] = {
    {"cpu", "CPU", 3, true, UL_TABLE_NUMBER},
    {"core", "CORE", 4, true, UL_TABLE_NULL_NUMBER},
    {"socket", "SOCKET", 6, true, UL_TABLE_NULL_NUMBER},
    {"cluster", "CLUSTER", 7, true, UL_TABLE_NULL_NUMBER},
    {"node", "NODE", 4, true, UL_TABLE_NULL_NUMBER},
    {"cache", "L1d:L1i:L2:L3", 0, false, UL_TABLE_STRING},
    {"online", "ONLINE", 6, true, UL_TABLE_BOOLEAN},
    {"bogomips", "BOGOMIPS", 0, true, UL_TABLE_NULL_NUMBER},
    {"microcode", "MICROCODE", 0, false, UL_TABLE_NULL_STRING},
    {"mhz", "MHZ", 0, true, UL_TABLE_NULL_NUMBER},
    {"scalmhz%", "SCALMHZ%", 0, true, UL_TABLE_NULL_NUMBER},
    {"maxmhz", "MAXMHZ", 0, true, UL_TABLE_NULL_NUMBER},
    {"minmhz", "MINMHZ", 0, true, UL_TABLE_NULL_NUMBER},
    {"modelname", "MODELNAME", 0, false, UL_TABLE_NULL_STRING},
};

static string_address ul_lscpu_frequency(p8 address_to text, positive khz)
{
        if (!khz)
                return (string_address)"";
        positive used = positive_into_string(text, khz / 1000);
        text[used++] = '.';
        used += positive_into_padded(text + used, khz % 1000, 3, '0');
        text[used++] = '0';
        text[used] = end;
        return text;
}

static string_address ul_lscpu_cpu_field(address_any row, p8 column,
                                         p8 address_to scratch)
{
        ul_lscpu_cpu address_to cpu = (ul_lscpu_cpu address_to)row;
        bipolar number = -1;
        switch (column)
        {
        case UL_LSCPU_CPU: number = (bipolar)cpu->id; break;
        case UL_LSCPU_CORE: number = cpu->core; break;
        case UL_LSCPU_SOCKET: number = cpu->socket; break;
        case UL_LSCPU_CLUSTER: number = cpu->cluster; break;
        case UL_LSCPU_NODE: number = cpu->node; break;
        case UL_LSCPU_CACHE:
        {
                positive used = 0;
                for (positive i = 0; i < ul_lscpu.cache_count; i++)
                {
                        if (i)
                                scratch[used++] = ':';
                        if (cpu->cache[i] >= 0)
                                used += positive_into_string(
                                    scratch + used, (positive)cpu->cache[i]);
                }
                scratch[used] = end;
                return scratch;
        }
        case UL_LSCPU_ONLINE:
                return cpu->online ? (string_address)"yes"
                                   : (string_address)"no";
        case UL_LSCPU_BOGOMIPS:
                return ul_lscpu.info.bogomips
                           ? ul_lscpu.info.bogomips : (string_address)"";
        case UL_LSCPU_MICROCODE:
                return ul_lscpu.info.microcode
                           ? ul_lscpu.info.microcode : (string_address)"";
        case UL_LSCPU_MHZ:
                return ul_lscpu_frequency(scratch, cpu->current_khz);
        case UL_LSCPU_SCALMHZ:
                if (!cpu->current_khz || !cpu->maximum_khz)
                        return (string_address)"";
                positive_into_string(
                    scratch, min((positive)100,
                                 cpu->current_khz * 100 / cpu->maximum_khz));
                return scratch;
        case UL_LSCPU_MAXMHZ:
                return ul_lscpu_frequency(scratch, cpu->maximum_khz);
        case UL_LSCPU_MINMHZ:
                return ul_lscpu_frequency(scratch, cpu->minimum_khz);
        default:
                return ul_lscpu.info.model_name
                           ? ul_lscpu.info.model_name : (string_address)"";
        }
        if (number < 0)
                return (string_address)"";
        positive_into_string(scratch, (positive)number);
        return scratch;
}

enum
{
        UL_LSCPU_C_NAME,
        UL_LSCPU_C_ONE,
        UL_LSCPU_C_ALL,
        UL_LSCPU_C_WAYS,
        UL_LSCPU_C_TYPE,
        UL_LSCPU_C_LEVEL,
        UL_LSCPU_C_SETS,
        UL_LSCPU_C_PHY,
        UL_LSCPU_C_COHERENCY,
        UL_LSCPU_C_COLUMNS,
};

static const ul_table_column ul_lscpu_cache_columns[] = {
    {"name", "NAME", 0, false, UL_TABLE_STRING},
    {"one-size", "ONE-SIZE", 0, true, UL_TABLE_NULL_STRING},
    {"all-size", "ALL-SIZE", 0, true, UL_TABLE_NULL_STRING},
    {"ways", "WAYS", 0, true, UL_TABLE_NULL_NUMBER},
    {"type", "TYPE", 11, false, UL_TABLE_STRING},
    {"level", "LEVEL", 0, true, UL_TABLE_NUMBER},
    {"sets", "SETS", 0, true, UL_TABLE_NULL_NUMBER},
    {"phy-line", "PHY-LINE", 0, true, UL_TABLE_NULL_NUMBER},
    {"coherency-size", "COHERENCY-SIZE", 0, true, UL_TABLE_NULL_NUMBER},
};

static bool ul_lscpu_bytes;

static string_address ul_lscpu_cache_field(address_any row, p8 column,
                                           p8 address_to scratch)
{
        ul_lscpu_cache address_to cache = (ul_lscpu_cache address_to)row;
        positive value = 0;
        switch (column)
        {
        case UL_LSCPU_C_NAME: return cache->name;
        case UL_LSCPU_C_ONE:
                if (!cache->size)
                        return (string_address)"";
                return ul_lscpu_cache_size(cache->size, ul_lscpu_bytes, false);
        case UL_LSCPU_C_ALL:
                if (!cache->size)
                        return (string_address)"";
                value = cache->size;
                if (cache->instances && value <= positive_max / cache->instances)
                        value *= cache->instances;
                return ul_lscpu_cache_size(value, ul_lscpu_bytes, false);
        case UL_LSCPU_C_WAYS: value = cache->ways; break;
        case UL_LSCPU_C_TYPE: return cache->type;
        case UL_LSCPU_C_LEVEL: value = cache->level; break;
        case UL_LSCPU_C_SETS: value = cache->sets; break;
        case UL_LSCPU_C_PHY: value = cache->physical_line; break;
        default: value = cache->coherency; break;
        }
        if (!value && column != UL_LSCPU_C_LEVEL)
                return (string_address)"";
        positive_into_string(scratch, value);
        return scratch;
}

static bool ul_lscpu_show(ul_lscpu_cpu address_to cpu, p8 filter)
{
        return filter == 'a' || (filter == 'b' && cpu->online) ||
               (filter == 'c' && !cpu->online);
}

static fn ul_lscpu_parse(p8 address_to columns, positive column_count,
                         bool defaults, p8 filter)
{
        string_address prelude =
            "# The following is the parsable format, which can be fed to other\n"
            "# programs. Each different item in every column has an unique ID\n"
            "# starting usually from zero.\n# ";
        log(prelude, string_length(prelude));
        if (defaults)
        {
                log("CPU,Core,", 9);
                string_address package = ul_lscpu.cluster_count
                                             ? "Cluster" : "Socket";
                log(package, string_length(package));
                log(",Node,", 6);
                for (positive i = 0; i < ul_lscpu.cache_count; i++)
                {
                        log(",", 1);
                        log(ul_lscpu.caches[i].name,
                            string_length(ul_lscpu.caches[i].name));
                }
        }
        else
                for (positive i = 0; i < column_count; i++)
                {
                        if (i)
                                log(",", 1);
                        string_address heading = ul_lscpu_columns[columns[i]].heading;
                        for (positive at = 0; string_get(heading + at); at++)
                        {
                                p8 byte = !at || columns[i] == UL_LSCPU_CPU
                                              ? heading[at]
                                              : byte_to_lower(heading[at]);
                                log(address_of byte, 1);
                        }
                }
        log("\n", 1);

        for (positive row = 0; row < ul_lscpu.cpu_count; row++)
        {
                ul_lscpu_cpu address_to cpu = ul_lscpu.cpus + row;
                if (!ul_lscpu_show(cpu, filter))
                        continue;
                if (defaults)
                {
                        positive_to_string(log, cpu->id);
                        log(",", 1);
                        if (cpu->core >= 0)
                                positive_to_string(log, (positive)cpu->core);
                        log(",", 1);
                        bipolar package = ul_lscpu.cluster_count
                                              ? cpu->cluster : cpu->socket;
                        if (package >= 0)
                                positive_to_string(log, (positive)package);
                        log(",", 1);
                        if (cpu->node >= 0)
                                positive_to_string(log, (positive)cpu->node);
                        log(",", 1);
                        for (positive i = 0; i < ul_lscpu.cache_count; i++)
                        {
                                log(",", 1);
                                if (cpu->cache[i] >= 0)
                                        positive_to_string(
                                            log, (positive)cpu->cache[i]);
                        }
                }
                else
                        for (positive i = 0; i < column_count; i++)
                        {
                                if (i)
                                        log(",", 1);
                                p8 scratch[96];
                                string_address value = ul_lscpu_cpu_field(
                                    cpu, columns[i], scratch);
                                log(value, string_length(value));
                        }
                log("\n", 1);
        }
}

static const file_long ul_lscpu_longs[] = {
    {"all", 'a'}, {"bytes", 'B'}, {"caches", 'C'},
    {"extended", 'e'}, {"hex", 'x'}, {"json", 'J'},
    {"offline", 'c'}, {"online", 'b'}, {"output", 'o'},
    {"parse", 'p'}, {"physical", 'y'}, {"raw", 'r'},
    {"sysroot", 's'}, {"list-columns", 'H'}, {"output-all", 'A'},
    {"help", 'h'}, {"version", 'V'}, {null, 0},
};

static bool ul_lscpu_unsupported_column(string_address text)
{
        while (string_get(text))
        {
                positive length = string_first_of(text, ',')
                                      ? (positive)(string_first_of(text, ',') - text)
                                      : string_length(text);
                if (file_same_word(text, length, "address") ||
                    file_same_word(text, length, "configured"))
                        return true;
                text += length;
                if (!string_get(text))
                        break;
                text++;
        }
        return false;
}

static b32 util_linux_lscpu()
{
        file_taking taking = {
            .program = "lscpu", .allowed = "aBbCcJeoprxysHAVh",
            .valued = "os", .optional = "peC", .sticky_optional = "peC",
            .longs = ul_lscpu_longs,
        };
        b32 answer;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options]", address_of answer))
                return answer;
        if (taking.first != (positive)program_argument_count())
                return ul_bad_usage("lscpu", "unexpected operand");
        if (taking.flags & FILE_FLAG('y'))
                return ul_bad_usage("lscpu",
                                    "physical identifiers are not supported");
        if (file_option_value(address_of taking, 's'))
                return ul_bad_usage("lscpu", "--sysroot is not supported");
        if (taking.flags & (FILE_FLAG('H') | FILE_FLAG('A')))
                return ul_bad_usage("lscpu", "column metadata is not supported");

        positive modes = ((taking.flags & FILE_FLAG('p')) != 0) +
                         ((taking.flags & FILE_FLAG('e')) != 0) +
                         ((taking.flags & FILE_FLAG('C')) != 0);
        positive filters = ((taking.flags & FILE_FLAG('a')) != 0) +
                           ((taking.flags & FILE_FLAG('b')) != 0) +
                           ((taking.flags & FILE_FLAG('c')) != 0);
        if (modes > 1)
                return ul_bad_usage("lscpu", "output modes are mutually exclusive");
        if (filters > 1)
                return ul_bad_usage("lscpu", "CPU filters are mutually exclusive");

        string_address selected = null;
        if (taking.flags & FILE_FLAG('p'))
                selected = file_option_value(address_of taking, 'p');
        else if (taking.flags & FILE_FLAG('e'))
                selected = file_option_value(address_of taking, 'e');
        else if (taking.flags & FILE_FLAG('C'))
                selected = file_option_value(address_of taking, 'C');
        string_address output = file_option_value(address_of taking, 'o');
        if (output && !modes)
                return ul_bad_usage("lscpu", "--output needs a table mode");
        if (output)
                selected = output;
        if (selected && string_is(selected, '='))
                selected++;
        if (selected && ul_lscpu_unsupported_column(selected))
                return ul_bad_usage(
                    "lscpu", "physical-address/configured columns are not supported");

        text_arena_used = 0;
        if (!ul_lscpu_take())
                return ul_bad_usage("lscpu", "cannot read CPU topology");
        ul_lscpu_columns[UL_LSCPU_CACHE].heading = ul_lscpu_cache_heading;
        bool json = (taking.flags & FILE_FLAG('J')) != 0;
        bool raw = (taking.flags & FILE_FLAG('r')) != 0;
        ul_lscpu_bytes = (taking.flags & FILE_FLAG('B')) != 0;
        p8 filter = taking.flags & FILE_FLAG('c') ? 'c'
                    : taking.flags & FILE_FLAG('b') ? 'b'
                    : taking.flags & FILE_FLAG('a') ? 'a'
                    : taking.flags & FILE_FLAG('p') ? 'b' : 'a';

        if (!modes)
                ul_lscpu_summary(json,
                                 (taking.flags & FILE_FLAG('x')) != 0,
                                 ul_lscpu_bytes);
        else if (taking.flags & FILE_FLAG('C'))
        {
                static const p8 defaults[] = {
                    UL_LSCPU_C_NAME, UL_LSCPU_C_ONE, UL_LSCPU_C_ALL,
                    UL_LSCPU_C_WAYS, UL_LSCPU_C_TYPE, UL_LSCPU_C_LEVEL,
                    UL_LSCPU_C_SETS, UL_LSCPU_C_PHY, UL_LSCPU_C_COHERENCY,
                };
                p8 columns[UL_LSCPU_C_COLUMNS];
                positive count = 0;
                if (selected &&
                    !ul_table_column_list(selected, ul_lscpu_cache_columns,
                                          UL_LSCPU_C_COLUMNS, defaults,
                                          array_count(defaults), columns,
                                          address_of count))
                        return ul_bad_usage("lscpu", "unknown cache column");
                if (!selected)
                        for (positive i = 0; i < array_count(defaults); i++)
                                columns[count++] = defaults[i];
                if (json)
                        ul_table_json("caches", ul_lscpu.caches,
                                      sizeof(ul_lscpu.caches[0]),
                                      ul_lscpu.cache_count,
                                      ul_lscpu_cache_columns, columns, count,
                                      ul_lscpu_cache_field);
                else
                        ul_table_out(ul_lscpu.caches,
                                     sizeof(ul_lscpu.caches[0]),
                                     ul_lscpu.cache_count,
                                     ul_lscpu_cache_columns,
                                     UL_LSCPU_C_COLUMNS, columns, count,
                                     true, raw, ul_lscpu_cache_field);
        }
        else
        {
                static const p8 socket_defaults[] = {
                    UL_LSCPU_CPU, UL_LSCPU_NODE, UL_LSCPU_SOCKET,
                    UL_LSCPU_CORE, UL_LSCPU_CACHE, UL_LSCPU_ONLINE,
                    UL_LSCPU_MAXMHZ, UL_LSCPU_MINMHZ, UL_LSCPU_MHZ,
                };
                static const p8 cluster_defaults[] = {
                    UL_LSCPU_CPU, UL_LSCPU_NODE, UL_LSCPU_CLUSTER,
                    UL_LSCPU_CORE, UL_LSCPU_CACHE, UL_LSCPU_ONLINE,
                };
                const p8 address_to defaults = ul_lscpu.cluster_count
                                                   ? cluster_defaults
                                                   : socket_defaults;
                positive default_count = ul_lscpu.cluster_count
                                             ? array_count(cluster_defaults)
                                             : array_count(socket_defaults);
                p8 columns[UL_LSCPU_COLUMNS];
                positive count = 0;
                if (selected &&
                    !ul_table_column_list(selected, ul_lscpu_columns,
                                          UL_LSCPU_COLUMNS, defaults,
                                          default_count, columns,
                                          address_of count))
                        return ul_bad_usage("lscpu", "unknown CPU column");
                if (!selected)
                        for (positive i = 0; i < default_count; i++)
                                columns[count++] = defaults[i];

                if (taking.flags & FILE_FLAG('p'))
                        ul_lscpu_parse(columns, count, !selected, filter);
                else
                {
                        positive shown = 0;
                        for (positive i = 0; i < ul_lscpu.cpu_count; i++)
                                if (ul_lscpu_show(ul_lscpu.cpus + i, filter))
                                        ul_lscpu.cpus[shown++] = ul_lscpu.cpus[i];
                        if (json)
                                ul_table_json("cpus", ul_lscpu.cpus,
                                              sizeof(ul_lscpu.cpus[0]), shown,
                                              ul_lscpu_columns, columns, count,
                                              ul_lscpu_cpu_field);
                        else
                                ul_table_out(ul_lscpu.cpus,
                                             sizeof(ul_lscpu.cpus[0]), shown,
                                             ul_lscpu_columns,
                                             UL_LSCPU_COLUMNS, columns, count,
                                             true, raw, ul_lscpu_cpu_field);
                }
        }
        log_flush();
        return 0;
}

// lsmem ------------------------------------------------------------

/* Memory hotplug exports the same kind of small, normalized sysfs records as
   CPU topology.  Keep one block snapshot and derive both coalesced ranges and
   every output projection from it; the shared arena and table formatter avoid
   another allocation, JSON, or smart-column layer. */
typedef struct
{
        positive id;
        string_address state;
        string_address zones;
        bipolar node;
        bool removable;
} ul_lsmem_block;

typedef struct
{
        positive first;
        positive last;
        string_address state;
        string_address zones;
        bipolar node;
        bool removable;
} ul_lsmem_range;

typedef struct
{
        ul_lsmem_block address_to blocks;
        ul_lsmem_range address_to ranges;
        positive block_count;
        positive range_count;
        positive block_size;
        positive online_size;
        positive offline_size;
        string_address memmap;
} ul_lsmem_snapshot;

static ul_lsmem_snapshot ul_lsmem;
static bool ul_lsmem_bytes;

enum
{
        UL_LSMEM_RANGE,
        UL_LSMEM_SIZE,
        UL_LSMEM_STATE,
        UL_LSMEM_REMOVABLE,
        UL_LSMEM_BLOCK,
        UL_LSMEM_NODE,
        UL_LSMEM_ZONES,
        UL_LSMEM_COLUMNS,
};

static const ul_table_column ul_lsmem_columns[] = {
    {"range", "RANGE", 0, false, UL_TABLE_STRING},
    {"size", "SIZE", 0, true, UL_TABLE_STRING},
    {"state", "STATE", 0, true, UL_TABLE_STRING},
    {"removable", "REMOVABLE", 0, true, UL_TABLE_BOOLEAN},
    {"block", "BLOCK", 0, true, UL_TABLE_STRING},
    {"node", "NODE", 0, true, UL_TABLE_NULL_NUMBER},
    {"zones", "ZONES", 0, true, UL_TABLE_NULL_STRING},
};

static ul_table_column ul_lsmem_summary_columns[] = {
    {"field", "FIELD", 32, false, UL_TABLE_STRING},
    {"data", "DATA", 0, true, UL_TABLE_STRING},
};

static bool ul_lsmem_id(string_address name, positive address_to id)
{
        return !string_compare_max(name, "memory", 6) &&
               ul_unsigned(name + 6, positive_max, id);
}

static fn ul_lsmem_path(p8 address_to path, positive id,
                        string_address property)
{
        string_address base =
            (string_address)"/sys/devices/system/memory/memory";
        positive at = string_length(base);
        memory_copy(path, base, at);
        at += positive_into_string(path + at, id);
        if (property)
        {
                path[at++] = '/';
                positive length = string_length(property);
                memory_copy(path + at, property, length + 1);
        }
        else
                path[at] = end;
}

static bipolar ul_lsmem_node(positive id)
{
        p8 directory[160];
        ul_lsmem_path(directory, id, null);
        file_walk walk;
        if (!file_walk_open(address_of walk, AT_FDCWD, directory))
                return -1;

        bipolar answer = -1;
        struct linux_dirent64 address_to entry;
        while ((entry = file_walk_next(address_of walk)))
        {
                string_address name = (string_address)entry->d_name;
                positive node;
                if (!string_compare_max(name, "node", 4) &&
                    ul_unsigned(name + 4, (positive)bipolar_max,
                                address_of node))
                {
                        answer = (bipolar)node;
                        break;
                }
        }
        file_walk_close(address_of walk);
        return answer;
}

static PURE bipolar ul_lsmem_order(ul_lsmem_block left,
                                    ul_lsmem_block right)
{
        if (left.id == right.id)
                return 0;
        return left.id < right.id ? -1 : 1;
}

static bool ul_lsmem_block_size(positive address_to value)
{
        p8 text[64];
        if (ul_slurp_word(
                (string_address)"/sys/devices/system/memory/block_size_bytes",
                text, sizeof(text)) <= 0)
                return false;
        string_address at = text;
        return string_digits_checked(address_of at, 16, value) &&
               !string_get(at) && address_to value;
}

static bool ul_lsmem_take()
{
        memory_fill(address_of ul_lsmem, 0, sizeof(ul_lsmem));
        if (!ul_lsmem_block_size(address_of ul_lsmem.block_size))
                return false;

        string_address root =
            (string_address)"/sys/devices/system/memory";
        file_walk walk;
        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return false;

        positive capacity = 0;
        struct linux_dirent64 address_to entry;
        while ((entry = file_walk_next(address_of walk)))
        {
                positive id;
                if (ul_lsmem_id((string_address)entry->d_name,
                                address_of id) &&
                    id <= positive_max / ul_lsmem.block_size)
                        capacity++;
        }
        file_walk_close(address_of walk);
        if (!capacity || capacity >
                (TEXT_ARENA_BYTES - text_arena_used) /
                    (sizeof(ul_lsmem_block) * 2 + sizeof(ul_lsmem_range)))
                return false;

        ul_lsmem.blocks = text_arena_take(
            capacity * sizeof(ul_lsmem_block));
        ul_lsmem_block address_to spare = text_arena_take(
            capacity * sizeof(ul_lsmem_block));
        ul_lsmem.ranges = text_arena_take(
            capacity * sizeof(ul_lsmem_range));
        if (!ul_lsmem.blocks || !spare || !ul_lsmem.ranges ||
            !file_walk_open(address_of walk, AT_FDCWD, root))
                return false;

        while (ul_lsmem.block_count < capacity &&
               (entry = file_walk_next(address_of walk)))
        {
                positive id;
                if (!ul_lsmem_id((string_address)entry->d_name,
                                 address_of id) ||
                    id > positive_max / ul_lsmem.block_size)
                        continue;

                ul_lsmem_block address_to block =
                    ul_lsmem.blocks + ul_lsmem.block_count++;
                memory_fill(block, 0, sizeof(*block));
                block->id = id;
                block->node = ul_lsmem_node(id);

                p8 path[192];
                p8 text[128];
                ul_lsmem_path(path, id, (string_address)"state");
                if (ul_slurp_word(path, text, sizeof(text)) > 0)
                        block->state = ul_lscpu_keep(text);
                else
                        block->state = (string_address)"unknown";

                positive removable;
                ul_lsmem_path(path, id, (string_address)"removable");
                block->removable =
                    ul_slurp_word(path, text, sizeof(text)) > 0 &&
                    ul_unsigned(text, 1, address_of removable) && removable;

                ul_lsmem_path(path, id, (string_address)"valid_zones");
                if (ul_slurp_word(path, text, sizeof(text)) > 0)
                {
                        if (string_equals(text, "none"))
                                block->zones = (string_address)"None";
                        else
                                block->zones = ul_lscpu_keep(text);
                }
                else
                        block->zones = (string_address)"";
        }
        file_walk_close(address_of walk);
        if (!ul_lsmem.block_count)
                return false;

        ul_lsmem.blocks = array_merge_sort(
            ul_lsmem.blocks, spare, ul_lsmem.block_count, ul_lsmem_order);
        for (positive i = 0; i < ul_lsmem.block_count; i++)
        {
                if (string_equals(ul_lsmem.blocks[i].state, "online"))
                        ul_lsmem.online_size += ul_lsmem.block_size;
                else
                        ul_lsmem.offline_size += ul_lsmem.block_size;
        }

        p8 parameter[16];
        if (ul_slurp_word(
                (string_address)"/sys/module/memory_hotplug/parameters/memmap_on_memory",
                parameter, sizeof(parameter)) > 0)
                ul_lsmem.memmap =
                    byte_to_lower(parameter[0]) == 'y'
                        ? (string_address)"yes" : (string_address)"no";
        return true;
}

static bool ul_lsmem_same(ul_lsmem_range address_to range,
                          ul_lsmem_block address_to block,
                          positive split)
{
        if (range->last == positive_max || block->id != range->last + 1)
                return false;
        if ((split & ((positive)1 << UL_LSMEM_STATE)) &&
            !string_equals(range->state, block->state))
                return false;
        if ((split & ((positive)1 << UL_LSMEM_REMOVABLE)) &&
            range->removable != block->removable)
                return false;
        if ((split & ((positive)1 << UL_LSMEM_NODE)) &&
            range->node != block->node)
                return false;
        if ((split & ((positive)1 << UL_LSMEM_ZONES)) &&
            !string_equals(range->zones, block->zones))
                return false;
        return true;
}

static fn ul_lsmem_ranges(positive split, bool every)
{
        ul_lsmem.range_count = 0;
        for (positive i = 0; i < ul_lsmem.block_count; i++)
        {
                ul_lsmem_block address_to block = ul_lsmem.blocks + i;
                ul_lsmem_range address_to range = ul_lsmem.range_count
                    ? ul_lsmem.ranges + ul_lsmem.range_count - 1 : null;
                if (!every && range && ul_lsmem_same(range, block, split))
                {
                        range->last = block->id;
                        continue;
                }
                range = ul_lsmem.ranges + ul_lsmem.range_count++;
                *range = (ul_lsmem_range){
                    .first = block->id,
                    .last = block->id,
                    .state = block->state,
                    .zones = block->zones,
                    .node = block->node,
                    .removable = block->removable,
                };
        }
}

static positive ul_lsmem_hex(p8 address_to text, positive value)
{
        memory_copy(text, "0x", 2);
        for (positive i = 0; i < 16; i++)
        {
                positive shift = (15 - i) * 4;
                text[2 + i] = storage_hex_digit((p8)(value >> shift & 15),
                                                 false);
        }
        return 18;
}

static string_address ul_lsmem_field(address_any row, p8 column,
                                     p8 address_to scratch)
{
        ul_lsmem_range address_to range = (ul_lsmem_range address_to)row;
        switch (column)
        {
        case UL_LSMEM_RANGE:
        {
                positive used = ul_lsmem_hex(
                    scratch, range->first * ul_lsmem.block_size);
                scratch[used++] = '-';
                used += ul_lsmem_hex(
                    scratch + used,
                    (range->last + 1) * ul_lsmem.block_size - 1);
                scratch[used] = end;
                return scratch;
        }
        case UL_LSMEM_SIZE:
                return ul_lscpu_cache_size(
                    (range->last - range->first + 1) * ul_lsmem.block_size,
                    ul_lsmem_bytes, false);
        case UL_LSMEM_STATE: return range->state;
        case UL_LSMEM_REMOVABLE:
                return range->removable ? (string_address)"yes"
                                        : (string_address)"no";
        case UL_LSMEM_BLOCK:
        {
                positive used = positive_into_string(scratch, range->first);
                if (range->last != range->first)
                {
                        scratch[used++] = '-';
                        used += positive_into_string(scratch + used,
                                                     range->last);
                }
                scratch[used] = end;
                return scratch;
        }
        case UL_LSMEM_NODE:
                if (range->node < 0)
                        return (string_address)"";
                positive_into_string(scratch, (positive)range->node);
                return scratch;
        default: return range->zones;
        }
}

static bool ul_lsmem_unsupported_column(string_address text)
{
        while (text && string_get(text))
        {
                if (string_is(text, '+'))
                        text++;
                string_address comma = string_first_of(text, ',');
                positive length = comma ? (positive)(comma - text)
                                        : string_length(text);
                if (file_same_word(text, length, "configured") ||
                    file_same_word(text, length, "memmap-on-memory"))
                        return true;
                text += length;
                if (!string_get(text))
                        break;
                text++;
        }
        return false;
}

static fn ul_lsmem_summary()
{
        ul_lscpu_summary_item items[4];
        positive count = 0;
        items[count++] = (ul_lscpu_summary_item){
            (string_address)"Memory block size:",
            ul_lscpu_cache_size(ul_lsmem.block_size, ul_lsmem_bytes, false)};
        items[count++] = (ul_lscpu_summary_item){
            (string_address)"Total online memory:",
            ul_lscpu_cache_size(ul_lsmem.online_size, ul_lsmem_bytes, false)};
        items[count++] = (ul_lscpu_summary_item){
            (string_address)"Total offline memory:",
            ul_lscpu_cache_size(ul_lsmem.offline_size, ul_lsmem_bytes, false)};
        if (ul_lsmem.memmap)
                items[count++] = (ul_lscpu_summary_item){
                    (string_address)"Memmap on memory parameter:",
                    ul_lsmem.memmap};
        ul_lsmem_summary_columns[0].width = ul_lsmem_bytes ? 36 : 32;
        p8 columns[] = {0, 1};
        ul_table_out(items, sizeof(items[0]), count,
                     ul_lsmem_summary_columns, 2, columns, 2,
                     false, false, ul_lscpu_summary_field);
}

static const file_long ul_lsmem_longs[] = {
    {"json", 'J'}, {"pairs", 'P'}, {"all", 'a'}, {"bytes", 'b'},
    {"noheadings", 'n'}, {"output", 'o'}, {"output-all", 'A'},
    {"raw", 'r'}, {"split", 'S'}, {"sysroot", 's'},
    {"summary", 'q'}, {"help", 'h'}, {"version", 'V'}, {null, 0},
};

static b32 util_linux_lsmem()
{
        file_taking taking = {
            .program = "lsmem", .allowed = "JPabnroASsqhV",
            .valued = "oSs", .optional = "q", .sticky_optional = "q",
            .longs = ul_lsmem_longs,
        };
        b32 answer;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options]", address_of answer))
                return answer;
        if (taking.first != (positive)program_argument_count())
                return ul_bad_usage("lsmem", "unexpected operand");
        if (taking.flags & FILE_FLAG('P'))
                return ul_bad_usage("lsmem", "--pairs is not supported");
        if (taking.flags & FILE_FLAG('A'))
                return ul_bad_usage("lsmem", "--output-all is not supported");
        if (file_option_value(address_of taking, 's'))
                return ul_bad_usage("lsmem", "--sysroot is not supported");

        string_address selected = file_option_value(address_of taking, 'o');
        string_address splitting = file_option_value(address_of taking, 'S');
        if (ul_lsmem_unsupported_column(selected) ||
            ul_lsmem_unsupported_column(splitting))
                return ul_bad_usage(
                    "lsmem", "configured/memmap columns are not supported");

        static const p8 defaults[] = {
            UL_LSMEM_RANGE, UL_LSMEM_SIZE, UL_LSMEM_STATE,
            UL_LSMEM_REMOVABLE, UL_LSMEM_BLOCK,
        };
        p8 columns[UL_LSMEM_COLUMNS];
        positive column_count = 0;
        if (selected &&
            !ul_table_column_list(selected, ul_lsmem_columns,
                                  UL_LSMEM_COLUMNS, defaults,
                                  array_count(defaults), columns,
                                  address_of column_count))
                return ul_bad_usage("lsmem", "unknown output column");
        if (!selected)
                for (positive i = 0; i < array_count(defaults); i++)
                        columns[column_count++] = defaults[i];

        positive split = ((positive)1 << UL_LSMEM_STATE) |
                         ((positive)1 << UL_LSMEM_REMOVABLE);
        for (positive i = 0; i < column_count; i++)
                if (columns[i] == UL_LSMEM_STATE ||
                    columns[i] == UL_LSMEM_REMOVABLE ||
                    columns[i] == UL_LSMEM_NODE ||
                    columns[i] == UL_LSMEM_ZONES)
                        split |= (positive)1 << columns[i];
        bool every = (taking.flags & FILE_FLAG('a')) != 0;
        if (splitting)
        {
                p8 split_columns[UL_LSMEM_COLUMNS];
                positive split_count = 0;
                if (!ul_table_column_list(splitting, ul_lsmem_columns,
                                          UL_LSMEM_COLUMNS, null, 0,
                                          split_columns, address_of split_count))
                        return ul_bad_usage("lsmem", "unknown split column");
                for (positive i = 0; i < split_count; i++)
                {
                        p8 column = split_columns[i];
                        if (column == UL_LSMEM_RANGE ||
                            column == UL_LSMEM_SIZE ||
                            column == UL_LSMEM_BLOCK)
                                every = true;
                        else
                                split |= (positive)1 << column;
                }
        }

        string_address summary = file_option_value(address_of taking, 'q');
        bool summary_requested =
            (taking.flags & FILE_FLAG('q')) != 0;
        bool summary_only = summary_requested &&
            (!summary || string_equals(summary, "only"));
        bool summary_never = summary && string_equals(summary, "never");
        bool summary_always = summary && string_equals(summary, "always");
        if (summary && !summary_only && !summary_never &&
            !summary_always)
                return ul_bad_usage("lsmem", "invalid --summary mode");
        bool json = (taking.flags & FILE_FLAG('J')) != 0;
        if (json && summary_only)
                return ul_bad_usage(
                    "lsmem", "JSON and summary-only output are incompatible");

        text_arena_used = 0;
        if (!ul_lsmem_take())
                return ul_bad_usage("lsmem", "memory hotplug sysfs is unavailable");
        ul_lsmem_ranges(split, every);
        ul_lsmem_bytes = (taking.flags & FILE_FLAG('b')) != 0;
        bool raw = (taking.flags & FILE_FLAG('r')) != 0;
        if (!summary_only)
        {
                if (json)
                        ul_table_json("memory", ul_lsmem.ranges,
                                      sizeof(ul_lsmem.ranges[0]),
                                      ul_lsmem.range_count, ul_lsmem_columns,
                                      columns, column_count, ul_lsmem_field);
                else
                        ul_table_out(ul_lsmem.ranges,
                                     sizeof(ul_lsmem.ranges[0]),
                                     ul_lsmem.range_count, ul_lsmem_columns,
                                     UL_LSMEM_COLUMNS, columns, column_count,
                                     !(taking.flags & FILE_FLAG('n')),
                                     raw,
                                     ul_lsmem_field);
        }
        if (!summary_never && ((!json && !raw) || summary_always))
        {
                if (!summary_only)
                        log("\n", 1);
                ul_lsmem_summary();
        }
        log_flush();
        return 0;
}

// lsblk ------------------------------------------------------------

#define UL_LSBLK_MOUNTS 16

typedef struct ul_lsblk_device ul_lsblk_device;
struct ul_lsblk_device
{
        string_address kname;
        string_address path;
        string_address type;
        string_address mountpoints[UL_LSBLK_MOUNTS];
        string_address mount_text;
        string_address fstype;
        string_address fsver;
        string_address label;
        string_address uuid;
        string_address partuuid;
        string_address partlabel;
        string_address owner;
        string_address group;
        string_address mode;
        string_address scheduler;
        string_address transport;
        string_address vendor;
        string_address model;
        string_address revision;
        string_address serial;
        string_address hctl;
        ul_lsblk_device address_to parent;
        positive major;
        positive minor;
        positive size;
        positive fs_available;
        positive fs_use;
        positive alignment;
        positive minimum_io;
        positive optimal_io;
        positive physical_sector;
        positive logical_sector;
        positive request_size;
        positive read_ahead;
        positive write_same;
        positive mount_count;
        positive depth;
        positive mode_bits;
        bool removable;
        bool read_only;
        bool rotational;
        bool partition;
        bool scsi;
        bool last;
        bool selected;
        bool fs_measured;
};

typedef struct
{
        ul_lsblk_device address_to devices;
        ul_lsblk_device address_to rows;
        positive capacity;
        positive count;
        positive row_count;
        bool failed;
} ul_lsblk_snapshot;

static ul_lsblk_snapshot ul_lsblk;
static bool ul_lsblk_bytes;
static bool ul_lsblk_tree;
static bool ul_lsblk_json;
static bool ul_lsblk_paths;

enum
{
        UL_LSBLK_NAME,
        UL_LSBLK_KNAME,
        UL_LSBLK_PATH,
        UL_LSBLK_MAJMIN,
        UL_LSBLK_RM,
        UL_LSBLK_SIZE,
        UL_LSBLK_RO,
        UL_LSBLK_TYPE,
        UL_LSBLK_MOUNTPOINT,
        UL_LSBLK_MOUNTPOINTS,
        UL_LSBLK_FSTYPE,
        UL_LSBLK_FSVER,
        UL_LSBLK_LABEL,
        UL_LSBLK_UUID,
        UL_LSBLK_PARTUUID,
        UL_LSBLK_PARTLABEL,
        UL_LSBLK_FSAVAIL,
        UL_LSBLK_FSUSE,
        UL_LSBLK_OWNER,
        UL_LSBLK_GROUP,
        UL_LSBLK_MODE,
        UL_LSBLK_ALIGNMENT,
        UL_LSBLK_MINIO,
        UL_LSBLK_OPTIO,
        UL_LSBLK_PHYSEC,
        UL_LSBLK_LOGSEC,
        UL_LSBLK_ROTA,
        UL_LSBLK_SCHED,
        UL_LSBLK_RQSIZE,
        UL_LSBLK_RA,
        UL_LSBLK_WSAME,
        UL_LSBLK_TRAN,
        UL_LSBLK_VENDOR,
        UL_LSBLK_MODEL,
        UL_LSBLK_REV,
        UL_LSBLK_SERIAL,
        UL_LSBLK_HCTL,
        UL_LSBLK_COLUMNS,
};

static const ul_table_column ul_lsblk_columns[] = {
    {"name", "NAME", 0, false, UL_TABLE_STRING},
    {"kname", "KNAME", 0, false, UL_TABLE_STRING},
    {"path", "PATH", 0, false, UL_TABLE_STRING},
    {"maj:min", "MAJ:MIN", 0, false, UL_TABLE_STRING},
    {"rm", "RM", 0, true, UL_TABLE_BOOLEAN},
    {"size", "SIZE", 4, true, UL_TABLE_STRING},
    {"ro", "RO", 0, true, UL_TABLE_BOOLEAN},
    {"type", "TYPE", 0, false, UL_TABLE_STRING},
    {"mountpoint", "MOUNTPOINT", 0, false, UL_TABLE_NULL_STRING},
    {"mountpoints", "MOUNTPOINTS", 0, false, UL_TABLE_NULL_STRING, true},
    {"fstype", "FSTYPE", 0, false, UL_TABLE_NULL_STRING},
    {"fsver", "FSVER", 0, false, UL_TABLE_NULL_STRING},
    {"label", "LABEL", 0, false, UL_TABLE_NULL_STRING},
    {"uuid", "UUID", 0, false, UL_TABLE_NULL_STRING},
    {"partuuid", "PARTUUID", 0, false, UL_TABLE_NULL_STRING},
    {"partlabel", "PARTLABEL", 0, false, UL_TABLE_NULL_STRING},
    {"fsavail", "FSAVAIL", 0, true, UL_TABLE_NULL_STRING},
    {"fsuse%", "FSUSE%", 0, true, UL_TABLE_NULL_STRING},
    {"owner", "OWNER", 0, false, UL_TABLE_NULL_STRING},
    {"group", "GROUP", 0, false, UL_TABLE_NULL_STRING},
    {"mode", "MODE", 0, false, UL_TABLE_NULL_STRING},
    {"alignment", "ALIGNMENT", 0, true, UL_TABLE_NUMBER},
    {"min-io", "MIN-IO", 0, true, UL_TABLE_NUMBER},
    {"opt-io", "OPT-IO", 0, true, UL_TABLE_NUMBER},
    {"phy-sec", "PHY-SEC", 0, true, UL_TABLE_NUMBER},
    {"log-sec", "LOG-SEC", 0, true, UL_TABLE_NUMBER},
    {"rota", "ROTA", 0, true, UL_TABLE_BOOLEAN},
    {"sched", "SCHED", 0, false, UL_TABLE_NULL_STRING},
    {"rq-size", "RQ-SIZE", 0, true, UL_TABLE_NULL_NUMBER},
    {"ra", "RA", 0, true, UL_TABLE_NULL_NUMBER},
    {"wsame", "WSAME", 0, true, UL_TABLE_STRING},
    {"tran", "TRAN", 0, false, UL_TABLE_NULL_STRING},
    {"vendor", "VENDOR", 0, false, UL_TABLE_NULL_STRING},
    {"model", "MODEL", 0, false, UL_TABLE_NULL_STRING},
    {"rev", "REV", 0, false, UL_TABLE_NULL_STRING},
    {"serial", "SERIAL", 0, false, UL_TABLE_NULL_STRING},
    {"hctl", "HCTL", 0, false, UL_TABLE_NULL_STRING},
};

static fn ul_lsblk_sysfs(p8 address_to path, string_address name,
                         string_address property)
{
        string_address base = (string_address)"/sys/class/block/";
        positive at = string_length(base);
        memory_copy(path, base, at);
        positive length = string_length(name);
        memory_copy(path + at, name, length);
        at += length;
        if (property)
        {
                path[at++] = '/';
                length = string_length(property);
                memory_copy(path + at, property, length + 1);
        }
        else
                path[at] = end;
}

static bool ul_lsblk_dev(string_address text, positive address_to major,
                         positive address_to minor)
{
        string_address at = text;
        return string_digits_checked(address_of at, 10, major) &&
               string_is(at, ':') && (++at) &&
               string_digits_checked(address_of at, 10, minor) &&
               !string_get(at);
}

static bool ul_lsblk_count_path(string_address path, address_any context)
{
        (void)path;
        positive address_to count = (positive address_to)context;
        (address_to count)++;
        return true;
}

static string_address ul_lsblk_word(string_address name,
                                    string_address property)
{
        p8 path[384];
        p8 text[512];
        ul_lsblk_sysfs(path, name, property);
        if (ul_slurp_word(path, text, sizeof(text)) <= 0)
                return (string_address)"";
        positive length = ul_trimmed(text, string_length(text));
        text[length] = end;
        return ul_lscpu_keep(text);
}

static positive ul_lsblk_number(string_address name,
                                string_address property)
{
        p8 path[384];
        positive value = 0;
        ul_lsblk_sysfs(path, name, property);
        (void)ul_lscpu_file_number(path, address_of value);
        return value;
}

static string_address ul_lsblk_scheduler(string_address text)
{
        string_address open = string_first_of(text, '[');
        if (!open)
                return text;
        string_address close = string_first_of(open + 1, ']');
        if (!close)
                return text;
        positive length = (positive)(close - open - 1);
        p8 copy[64];
        length = min(length, sizeof(copy) - 1);
        memory_copy(copy, open + 1, length);
        copy[length] = end;
        return ul_lscpu_keep(copy);
}

static bool ul_lsblk_take_path(string_address path, address_any context)
{
        (void)context;
        if (ul_lsblk.count == ul_lsblk.capacity)
        {
                ul_lsblk.failed = true;
                return false;
        }
        string_address name = path + sizeof("/dev/") - 1;
        ul_lsblk_device address_to device =
            ul_lsblk.devices + ul_lsblk.count++;
        memory_fill(device, 0, sizeof(*device));
        device->kname = ul_lscpu_keep(name);
        device->path = ul_lscpu_keep(path);
        device->parent = null;

        p8 sysfs[384];
        p8 text[64];
        ul_lsblk_sysfs(sysfs, name, (string_address)"dev");
        if (ul_slurp_word(sysfs, text, sizeof(text)) <= 0 ||
            !ul_lsblk_dev(text, address_of device->major,
                          address_of device->minor))
                return true;
        positive sectors = ul_lsblk_number(name, (string_address)"size");
        if (sectors <= positive_max / 512)
                device->size = sectors * 512;
        device->read_only = ul_lsblk_number(name, (string_address)"ro") != 0;
        device->removable =
            ul_lsblk_number(name, (string_address)"removable") != 0;
        device->partition =
            ul_lsblk_number(name, (string_address)"partition") != 0;
        device->alignment =
            ul_lsblk_number(name, (string_address)"alignment_offset");
        device->minimum_io =
            ul_lsblk_number(name, (string_address)"queue/minimum_io_size");
        device->optimal_io =
            ul_lsblk_number(name, (string_address)"queue/optimal_io_size");
        device->physical_sector =
            ul_lsblk_number(name, (string_address)"queue/physical_block_size");
        device->logical_sector =
            ul_lsblk_number(name, (string_address)"queue/logical_block_size");
        device->rotational =
            ul_lsblk_number(name, (string_address)"queue/rotational") != 0;
        device->request_size =
            ul_lsblk_number(name, (string_address)"queue/nr_requests");
        device->read_ahead =
            ul_lsblk_number(name, (string_address)"queue/read_ahead_kb");
        device->write_same =
            ul_lsblk_number(name, (string_address)"queue/write_same_max_bytes");
        device->scheduler = ul_lsblk_scheduler(
            ul_lsblk_word(name, (string_address)"queue/scheduler"));

        if (device->partition)
                device->type = (string_address)"part";
        else if (!string_compare_max(name, "loop", 4))
                device->type = (string_address)"loop";
        else if (!string_compare_max(name, "sr", 2))
                device->type = (string_address)"rom";
        else
                device->type = (string_address)"disk";
        return true;
}

static PURE bipolar ul_lsblk_order(ul_lsblk_device left,
                                    ul_lsblk_device right)
{
        if (left.major != right.major)
                return left.major < right.major ? -1 : 1;
        if (left.minor != right.minor)
                return left.minor < right.minor ? -1 : 1;
        return string_compare(left.kname, right.kname);
}

static ul_lsblk_device address_to ul_lsblk_find(string_address name)
{
        for (positive i = 0; i < ul_lsblk.count; i++)
                if (string_equals(ul_lsblk.devices[i].kname, name))
                        return ul_lsblk.devices + i;
        return null;
}

static fn ul_lsblk_parents()
{
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                ul_lsblk_device address_to device = ul_lsblk.devices + i;
                if (!device->partition)
                        continue;
                p8 path[384];
                p8 target[512];
                ul_lsblk_sysfs(path, device->kname, null);
                bipolar got = system_read_link_at(AT_FDCWD, path, target,
                                                   sizeof(target) - 1);
                if (got <= 0 || (positive)got >= sizeof(target))
                        continue;
                target[got] = end;
                p8 address_to tail = (p8 address_to)string_last_of(target, '/');
                if (!tail)
                        continue;
                *tail = end;
                p8 address_to parent =
                    (p8 address_to)string_last_of(target, '/');
                parent = parent ? parent + 1 : target;
                device->parent = ul_lsblk_find(parent);
        }
        /* Device-mapper, MD and similar stacked devices publish their upward
           edges in holders.  Partitions already have the more precise parent
           from their class symlink; retain the first holder edge on genuinely
           multi-parent graphs so traversal stays finite and deterministic. */
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                p8 path[384];
                ul_lsblk_sysfs(path, ul_lsblk.devices[i].kname,
                               (string_address)"holders");
                file_walk walk;
                if (!file_walk_open(address_of walk, AT_FDCWD, path))
                        continue;
                struct linux_dirent64 address_to entry;
                while ((entry = file_walk_next(address_of walk)))
                {
                        ul_lsblk_device address_to holder = ul_lsblk_find(
                            (string_address)entry->d_name);
                        if (holder && holder != ul_lsblk.devices + i &&
                            !holder->parent)
                                holder->parent = ul_lsblk.devices + i;
                }
                file_walk_close(address_of walk);
        }
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                ul_lsblk_device address_to device = ul_lsblk.devices + i;
                if (!device->parent)
                        continue;
                ul_lsblk_device address_to parent = device->parent;
                if (!device->minimum_io) device->minimum_io = parent->minimum_io;
                if (!device->physical_sector) device->physical_sector = parent->physical_sector;
                if (!device->logical_sector) device->logical_sector = parent->logical_sector;
                if (!device->request_size) device->request_size = parent->request_size;
                if (!device->read_ahead) device->read_ahead = parent->read_ahead;
                if (!string_get(device->scheduler)) device->scheduler = parent->scheduler;
                device->rotational = parent->rotational;
                device->removable = parent->removable;
        }
}

static positive ul_lsblk_percent(p64 part, p64 whole)
{
        if (!whole || part >= whole)
                return part ? 100 : 0;
        p64 remainder = 0;
        positive percent = 0;
        for (positive i = 0; i < 100; i++)
                if (remainder >= whole - part)
                {
                        remainder -= whole - part;
                        percent++;
                }
                else
                        remainder += part;
        if (remainder >= whole / 2 + whole % 2)
                percent++;
        return percent;
}

static fn ul_lsblk_mounts()
{
        storage_mount_table mounts;
        if (storage_mount_table_load(address_of mounts, null))
        {
                for (positive i = 0; i < mounts.count; i++)
                {
                        positive major;
                        positive minor;
                        if (!ul_lsblk_dev(mounts.entry[i].device,
                                          address_of major, address_of minor))
                                continue;
                        for (positive j = 0; j < ul_lsblk.count; j++)
                        {
                                ul_lsblk_device address_to device =
                                    ul_lsblk.devices + j;
                                if (device->major != major ||
                                    device->minor != minor ||
                                    device->mount_count == UL_LSBLK_MOUNTS)
                                        continue;
                                string_address target =
                                    ul_lscpu_keep(mounts.entry[i].target);
                                /* libmount reports the most recently mounted
                                   path first.  The shared mount snapshot is in
                                   kernel order, so prepend each bounded entry. */
                                for (positive mount = device->mount_count;
                                     mount; mount--)
                                        device->mountpoints[mount] =
                                            device->mountpoints[mount - 1];
                                device->mountpoints[0] = target;
                                device->mount_count++;
                                if (!device->fs_measured)
                                {
                                        file_mount_facts facts;
                                        memory_fill(address_of facts, 0,
                                                    sizeof(facts));
                                        if (system_call_2(
                                                syscall(statfs),
                                                (positive)target,
                                                (positive)address_of facts) >= 0)
                                        {
                                                positive unit = facts.fragment_size
                                                    ? (positive)facts.fragment_size
                                                    : (positive)facts.block_size;
                                                if (unit && facts.blocks_available <=
                                                        positive_max / unit)
                                                        device->fs_available =
                                                            facts.blocks_available * unit;
                                                p64 used = facts.blocks - facts.blocks_free;
                                                if (facts.blocks)
                                                        device->fs_use = ul_lsblk_percent(
                                                            used, facts.blocks);
                                                device->fs_measured = true;
                                        }
                                }
                                break;
                        }
                }
                storage_mount_table_release(address_of mounts);
        }

        byte_store swaps = {0};
        if (file_store_slurp((string_address)"/proc/swaps", address_of swaps))
        {
                p8 address_to cursor = swaps.bytes;
                p8 address_to limit = cursor + swaps.used;
                p8 address_to line = storage_line_next(address_of cursor, limit);
                (void)line;
                while ((line = storage_line_next(address_of cursor, limit)))
                {
                        p8 address_to at = line;
                        string_address path = storage_field(address_of at);
                        if (!path)
                                continue;
                        for (positive i = 0; i < ul_lsblk.count; i++)
                                if (string_equals(path, ul_lsblk.devices[i].path) &&
                                    ul_lsblk.devices[i].mount_count < UL_LSBLK_MOUNTS)
                                {
                                        ul_lsblk.devices[i].mountpoints[
                                            ul_lsblk.devices[i].mount_count++] =
                                                (string_address)"[SWAP]";
                                        break;
                                }
                }
                byte_store_release(address_of swaps);
        }
}

static bool ul_lsblk_mount_texts()
{
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                ul_lsblk_device address_to device = ul_lsblk.devices + i;
                if (!device->mount_count)
                        continue;
                if (device->mount_count == 1)
                {
                        device->mount_text = device->mountpoints[0];
                        continue;
                }
                positive bytes = 1;
                for (positive mount = 0; mount < device->mount_count; mount++)
                {
                        positive length = string_length(
                            device->mountpoints[mount]);
                        if (bytes > positive_max - length - (mount != 0))
                                return false;
                        bytes += length + (mount != 0);
                }
                p8 address_to joined = text_arena_take(bytes);
                if (!joined)
                        return false;
                positive used = 0;
                for (positive mount = 0; mount < device->mount_count; mount++)
                {
                        if (mount) joined[used++] = '\n';
                        positive length = string_length(
                            device->mountpoints[mount]);
                        memory_copy(joined + used, device->mountpoints[mount],
                                    length);
                        used += length;
                }
                joined[used] = end;
                device->mount_text = joined;
        }
        return true;
}

static fn ul_lsblk_identity(ul_lsblk_device address_to device,
                            bool probe_identity)
{
        p8 path[96];
        p8 text[16384];
        positive used = sizeof("/run/udev/data/b") - 1;
        memory_copy(path, "/run/udev/data/b", used);
        used += positive_into_string(path + used, device->major);
        path[used++] = ':';
        used += positive_into_string(path + used, device->minor);
        path[used] = end;
        bipolar got = file_slurp(path, text, sizeof(text) - 1);
        if (got > 0)
        {
                text[got] = end;
                p8 address_to line = text;
                p8 address_to limit = text + got;
                while (line < limit)
                {
                        p8 address_to newline = (p8 address_to)memory_first_of(
                            line, '\n', (positive)(limit - line));
                        p8 address_to stop = newline ? newline : limit;
                        if (stop > line + 3 && line[0] == 'E' && line[1] == ':')
                        {
                                p8 address_to equal = (p8 address_to)memory_first_of(
                                    line + 2, '=', (positive)(stop - line - 2));
                                if (equal)
                                {
                                        p8 saved = *stop;
                                        *stop = end;
                                        string_address value = equal + 1;
                                        positive length = (positive)(equal - line - 2);
#define UL_LSBLK_UDEV(key, member)                                           \
        if (file_same_word(line + 2, length, key))                           \
                device->member = ul_lscpu_keep(value);                       \
        else
                                        UL_LSBLK_UDEV("ID_FS_TYPE", fstype)
                                        UL_LSBLK_UDEV("ID_FS_VERSION", fsver)
                                        UL_LSBLK_UDEV("ID_FS_LABEL", label)
                                        UL_LSBLK_UDEV("ID_FS_UUID", uuid)
                                        UL_LSBLK_UDEV("ID_PART_ENTRY_UUID", partuuid)
                                        UL_LSBLK_UDEV("ID_PART_ENTRY_NAME", partlabel)
                                        UL_LSBLK_UDEV("ID_BUS", transport)
                                        UL_LSBLK_UDEV("ID_VENDOR", vendor)
                                        UL_LSBLK_UDEV("ID_MODEL", model)
                                        UL_LSBLK_UDEV("ID_REVISION", revision)
                                        UL_LSBLK_UDEV("ID_SERIAL_SHORT", serial)
                                                (void)value;
#undef UL_LSBLK_UDEV
                                        *stop = saved;
                                }
                        }
                        line = newline ? newline + 1 : limit;
                }
        }

        if (!probe_identity)
                return;
        storage_identity identity;
        if (!storage_probe_device(device->path, address_of identity))
                return;
        if (!device->fstype && identity.type_length)
                device->fstype = ul_lscpu_keep(identity.type);
        if (!device->uuid && identity.uuid_length)
                device->uuid = ul_lscpu_keep(identity.uuid);
        if (!device->label && identity.label_length)
                device->label = ul_lscpu_keep(identity.label);
        if (!device->partuuid && identity.partuuid_length)
                device->partuuid = ul_lscpu_keep(identity.partuuid);
        if (!device->partlabel && identity.partlabel_length)
                device->partlabel = ul_lscpu_keep(identity.partlabel);
}

static fn ul_lsblk_permissions(ul_lsblk_device address_to device)
{
        file_facts facts;
        if (!file_look_at(device->path, address_of facts))
                return;
        p8 text[FILE_NAME_MAX];
        file_account_label(facts.owner, false, true, text);
        device->owner = ul_lscpu_keep(text);
        file_account_label(facts.group, true, true, text);
        device->group = ul_lscpu_keep(text);
        file_mode_letters(text, facts.mode);
        device->mode = ul_lscpu_keep(text);
        device->mode_bits = facts.mode;
}

static string_address ul_lsblk_link_word(string_address name,
                                         string_address property)
{
        p8 path[384];
        p8 target[512];
        ul_lsblk_sysfs(path, name, property);
        bipolar got = system_read_link_at(AT_FDCWD, path, target,
                                           sizeof(target) - 1);
        if (got <= 0 || (positive)got >= sizeof(target))
                return (string_address)"";
        target[got] = end;
        string_address tail = string_last_of(target, '/');
        return ul_lscpu_keep(tail ? tail + 1 : target);
}

static bool ul_lsblk_hctl_valid(string_address text)
{
        for (positive field = 0; field < 4; field++)
        {
                positive number;
                if (!string_digits_checked(address_of text, 10,
                                           address_of number))
                        return false;
                if (field == 3)
                        return !string_get(text);
                if (!string_is(text, ':'))
                        return false;
                text++;
        }
        return false;
}

static fn ul_lsblk_scsi(ul_lsblk_device address_to device)
{
        string_address word = ul_lsblk_word(
            device->kname, (string_address)"device/vendor");
        if (string_get(word)) device->vendor = word;
        word = ul_lsblk_word(device->kname, (string_address)"device/model");
        if (string_get(word)) device->model = word;
        word = ul_lsblk_word(device->kname, (string_address)"device/rev");
        if (!string_get(word))
                word = ul_lsblk_word(device->kname,
                                     (string_address)"device/firmware_rev");
        if (string_get(word)) device->revision = word;
        word = ul_lsblk_word(device->kname, (string_address)"device/serial");
        if (string_get(word)) device->serial = word;

        string_address kind = ul_lsblk_word(device->kname,
                                            (string_address)"device/type");
        device->scsi = string_get(kind) != 0;
        if (!device->transport)
        {
                word = ul_lsblk_link_word(
                    device->kname, (string_address)"device/subsystem");
                if (string_get(word)) device->transport = word;
        }
        word = ul_lsblk_link_word(device->kname, (string_address)"device");
        if (ul_lsblk_hctl_valid(word)) device->hctl = word;
}

static bool ul_lsblk_take(bool identity, bool permissions, bool metadata)
{
        memory_fill(address_of ul_lsblk, 0, sizeof(ul_lsblk));
        storage_each_block_path(ul_lsblk_count_path, address_of ul_lsblk.capacity);
        if (!ul_lsblk.capacity ||
            ul_lsblk.capacity > (TEXT_ARENA_BYTES - text_arena_used) /
                                    (sizeof(ul_lsblk_device) * 3))
                return false;
        ul_lsblk.devices = text_arena_take(
            ul_lsblk.capacity * sizeof(ul_lsblk_device));
        ul_lsblk_device address_to spare = text_arena_take(
            ul_lsblk.capacity * sizeof(ul_lsblk_device));
        ul_lsblk.rows = text_arena_take(
            ul_lsblk.capacity * sizeof(ul_lsblk_device));
        if (!ul_lsblk.devices || !spare || !ul_lsblk.rows)
                return false;
        storage_each_block_path(ul_lsblk_take_path, null);
        if (ul_lsblk.failed || !ul_lsblk.count)
                return false;
        ul_lsblk.devices = array_merge_sort(ul_lsblk.devices, spare,
                                            ul_lsblk.count, ul_lsblk_order);
        ul_lsblk_parents();
        ul_lsblk_mounts();
        if (!ul_lsblk_mount_texts())
                return false;
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                if (identity || metadata)
                        ul_lsblk_identity(ul_lsblk.devices + i, identity);
                if (permissions) ul_lsblk_permissions(ul_lsblk.devices + i);
                if (metadata) ul_lsblk_scsi(ul_lsblk.devices + i);
        }
        if (metadata)
                for (positive i = 0; i < ul_lsblk.count; i++)
                {
                        ul_lsblk_device address_to device =
                            ul_lsblk.devices + i;
                        if (!device->transport && device->parent)
                                device->transport = device->parent->transport;
                }
        return true;
}

static string_address ul_lsblk_field(address_any row, p8 column,
                                     p8 address_to scratch)
{
        ul_lsblk_device address_to device = (ul_lsblk_device address_to)row;
        string_address blank = (string_address)"";
        switch (column)
        {
        case UL_LSBLK_NAME:
        {
                string_address name = ul_lsblk_paths ? device->path
                                                     : device->kname;
                if (!ul_lsblk_tree || ul_lsblk_json || !device->depth)
                        return name;
                positive used = 0;
                for (positive i = 1; i < device->depth; i++)
                {
                        scratch[used++] = '|';
                        scratch[used++] = ' ';
                }
                scratch[used++] = device->last ? '`' : '|';
                scratch[used++] = '-';
                positive length = string_length(name);
                memory_copy(scratch + used, name, length + 1);
                return scratch;
        }
        case UL_LSBLK_KNAME: return device->kname;
        case UL_LSBLK_PATH: return device->path;
        case UL_LSBLK_MAJMIN:
        {
                positive used = positive_into_string(scratch, device->major);
                scratch[used++] = ':';
                used += positive_into_string(scratch + used, device->minor);
                scratch[used] = end;
                return scratch;
        }
        case UL_LSBLK_RM: return device->removable ? (string_address)"1" : (string_address)"0";
        case UL_LSBLK_SIZE:
                return ul_lscpu_cache_size(device->size, ul_lsblk_bytes,
                                            false);
        case UL_LSBLK_RO: return device->read_only ? (string_address)"1" : (string_address)"0";
        case UL_LSBLK_TYPE: return device->type ? device->type : blank;
        case UL_LSBLK_MOUNTPOINT:
                return device->mount_count ? device->mountpoints[0] : blank;
        case UL_LSBLK_MOUNTPOINTS:
                return device->mount_text ? device->mount_text : blank;
        case UL_LSBLK_FSTYPE: return device->fstype ? device->fstype : blank;
        case UL_LSBLK_FSVER: return device->fsver ? device->fsver : blank;
        case UL_LSBLK_LABEL: return device->label ? device->label : blank;
        case UL_LSBLK_UUID: return device->uuid ? device->uuid : blank;
        case UL_LSBLK_PARTUUID: return device->partuuid ? device->partuuid : blank;
        case UL_LSBLK_PARTLABEL: return device->partlabel ? device->partlabel : blank;
        case UL_LSBLK_FSAVAIL:
                if (!device->fs_measured) return blank;
                /* util-linux keeps a measured zero unitless (including in
                   the human-size mode), rather than spelling it as 0B. */
                if (!device->fs_available) return (string_address)"0";
                return ul_lscpu_cache_size(device->fs_available,
                                            ul_lsblk_bytes, false);
        case UL_LSBLK_FSUSE:
                if (!device->fs_measured) return blank;
                positive_into_string(scratch, device->fs_use);
                string_copy_end(scratch + string_length(scratch), "%");
                return scratch;
        case UL_LSBLK_OWNER: return device->owner ? device->owner : blank;
        case UL_LSBLK_GROUP: return device->group ? device->group : blank;
        case UL_LSBLK_MODE: return device->mode ? device->mode : blank;
        case UL_LSBLK_SCHED:
                return device->scheduler ? device->scheduler : blank;
        case UL_LSBLK_TRAN:
                return device->transport ? device->transport : blank;
        case UL_LSBLK_VENDOR: return device->vendor ? device->vendor : blank;
        case UL_LSBLK_MODEL: return device->model ? device->model : blank;
        case UL_LSBLK_REV: return device->revision ? device->revision : blank;
        case UL_LSBLK_SERIAL: return device->serial ? device->serial : blank;
        case UL_LSBLK_HCTL: return device->hctl ? device->hctl : blank;
        case UL_LSBLK_WSAME:
                return ul_lscpu_cache_size(device->write_same,
                                            ul_lsblk_bytes, false);
        default:
        {
                positive value = 0;
                switch (column)
                {
                case UL_LSBLK_ALIGNMENT: value = device->alignment; break;
                case UL_LSBLK_MINIO: value = device->minimum_io; break;
                case UL_LSBLK_OPTIO: value = device->optimal_io; break;
                case UL_LSBLK_PHYSEC: value = device->physical_sector; break;
                case UL_LSBLK_LOGSEC: value = device->logical_sector; break;
                case UL_LSBLK_ROTA: value = device->rotational; break;
                case UL_LSBLK_RQSIZE: value = device->request_size; break;
                default: value = device->read_ahead; break;
                }
                if (column == UL_LSBLK_RQSIZE && !value)
                        return blank;
                positive_into_string(scratch, value);
                return scratch;
        }
        }
}

static fn ul_lsblk_append(ul_lsblk_device address_to device,
                          positive depth, bool last, bool dependencies)
{
        if (ul_lsblk.row_count == ul_lsblk.capacity)
                return;
        ul_lsblk.rows[ul_lsblk.row_count] = *device;
        ul_lsblk.rows[ul_lsblk.row_count].depth = depth;
        ul_lsblk.rows[ul_lsblk.row_count].last = last;
        ul_lsblk.row_count++;
        if (!dependencies || depth >= 32)
                return;

        positive children = 0;
        for (positive i = 0; i < ul_lsblk.count; i++)
                if (ul_lsblk.devices[i].parent == device)
                        children++;
        positive seen = 0;
        for (positive i = 0; i < ul_lsblk.count; i++)
                if (ul_lsblk.devices[i].parent == device)
                        ul_lsblk_append(ul_lsblk.devices + i, depth + 1,
                                        ++seen == children, true);
}

static bool ul_lsblk_under_selection(ul_lsblk_device address_to device)
{
        for (positive depth = 0; device && depth < 32;
             depth++, device = device->parent)
                if (device->selected)
                        return true;
        return false;
}

static bool ul_lsblk_select_operands(positive first)
{
        positive count = (positive)program_argument_count();
        if (first == count)
        {
                for (positive i = 0; i < ul_lsblk.count; i++)
                        ul_lsblk.devices[i].selected = true;
                return true;
        }

        bool failed = false;
        for (positive at = first; at < count; at++)
        {
                string_address operand = program_argument((b32)at);
                file_facts facts;
                ul_lsblk_device address_to found = null;
                if (file_look_at(operand, address_of facts))
                        for (positive i = 0; i < ul_lsblk.count; i++)
                                if (facts.rdev_major == ul_lsblk.devices[i].major &&
                                    facts.rdev_minor == ul_lsblk.devices[i].minor)
                                {
                                        found = ul_lsblk.devices + i;
                                        break;
                                }
                if (!found)
                {
                        string_address slash = string_last_of(operand, '/');
                        found = ul_lsblk_find(slash ? slash + 1 : operand);
                }
                if (!found)
                {
                        string_format(file_fail, "lsblk: %s: not a block device\n",
                                      operand);
                        failed = true;
                }
                else
                        found->selected = true;
        }
        return !failed;
}

static fn ul_lsblk_rows(bool list, bool dependencies, bool all, bool noempty,
                        bool scsi)
{
        ul_lsblk.row_count = 0;
        if (list)
        {
                for (positive i = 0; i < ul_lsblk.count; i++)
                {
                        ul_lsblk_device address_to device = ul_lsblk.devices + i;
                        if (!ul_lsblk_under_selection(device) ||
                            (scsi && !device->scsi) ||
                            ((!all || noempty) && !device->size &&
                             !string_compare_max(device->kname, "loop", 4)))
                                continue;
                        ul_lsblk.rows[ul_lsblk.row_count] = *device;
                        ul_lsblk.rows[ul_lsblk.row_count].depth = 0;
                        ul_lsblk.row_count++;
                }
                return;
        }

        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                ul_lsblk_device address_to device = ul_lsblk.devices + i;
                if (device->parent || !device->selected ||
                    (scsi && !device->scsi) ||
                    ((!all || noempty) && !device->size &&
                     !string_compare_max(device->kname, "loop", 4)))
                        continue;
                ul_lsblk_append(device, 0, false, dependencies);
        }
        /* A named partition is its own root rather than disappearing because
           its physical parent was not selected. */
        for (positive i = 0; i < ul_lsblk.count; i++)
        {
                ul_lsblk_device address_to device = ul_lsblk.devices + i;
                if (device->parent && device->selected &&
                    !(device->parent->selected))
                        ul_lsblk_append(device, 0, false, dependencies);
        }
}

static fn ul_lsblk_json_scalar(ul_lsblk_device address_to device, p8 column)
{
        p8 scratch[96];
        string_address value = ul_lsblk_field(device, column, scratch);
        p8 kind = ul_lsblk_columns[column].json;
        if ((kind == UL_TABLE_NULL_STRING || kind == UL_TABLE_NULL_NUMBER) &&
            !string_get(value))
                log("null", 4);
        else if (kind == UL_TABLE_BOOLEAN)
                log(string_equals(value, "0") ? "false" : "true",
                    string_equals(value, "0") ? 5 : 4);
        else if (kind == UL_TABLE_NUMBER || kind == UL_TABLE_NULL_NUMBER)
                log(value, string_length(value));
        else
                ul_lsns_json_string(value);
}

static positive ul_lsblk_json_row(positive row, p8 address_to columns,
                                   positive column_count, positive indent)
{
        ul_lsblk_device address_to device = ul_lsblk.rows + row;
        writer_fill(log, indent, ' ');
        log("{\n", 2);
        for (positive i = 0; i < column_count; i++)
        {
                p8 column = columns[i];
                if (i)
                        log(",\n", 2);
                writer_fill(log, indent + 3, ' ');
                string_format(log, "\"%s\": ", ul_lsblk_columns[column].name);
                if (column == UL_LSBLK_MOUNTPOINTS)
                {
                        log("[", 1);
                        for (positive m = 0; m < device->mount_count; m++)
                        {
                                if (m) log(",", 1);
                                ul_lsns_json_string(device->mountpoints[m]);
                        }
                        log("]", 1);
                }
                else
                        ul_lsblk_json_scalar(device, column);
        }

        positive next = row + 1;
        if (ul_lsblk_tree && next < ul_lsblk.row_count &&
            ul_lsblk.rows[next].depth > device->depth)
        {
                log(",\n", 2);
                writer_fill(log, indent + 3, ' ');
                log("\"children\": [\n", 14);
                bool comma = false;
                while (next < ul_lsblk.row_count &&
                       ul_lsblk.rows[next].depth > device->depth)
                {
                        if (comma) log(",\n", 2);
                        next = ul_lsblk_json_row(next, columns, column_count,
                                                indent + 6);
                        comma = true;
                }
                log("\n", 1);
                writer_fill(log, indent + 3, ' ');
                log("]", 1);
        }
        log("\n", 1);
        writer_fill(log, indent, ' ');
        log("}", 1);
        return next;
}

static fn ul_lsblk_json_out(p8 address_to columns, positive column_count)
{
        log("{\n   \"blockdevices\": [",
            sizeof("{\n   \"blockdevices\": [") - 1);
        positive row = 0;
        bool comma = false;
        while (row < ul_lsblk.row_count)
        {
                log(comma ? ",\n" : "\n", comma ? 2 : 1);
                row = ul_lsblk_json_row(row, columns, column_count, 6);
                comma = true;
        }
        log("\n   ]\n}\n", sizeof("\n   ]\n}\n") - 1);
}

static bool ul_lsblk_column_needs_identity(p8 column)
{
        return column == UL_LSBLK_FSTYPE || column == UL_LSBLK_FSVER ||
               column == UL_LSBLK_LABEL || column == UL_LSBLK_UUID ||
               column == UL_LSBLK_PARTUUID || column == UL_LSBLK_PARTLABEL;
}

static bool ul_lsblk_column_needs_permissions(p8 column)
{
        return column == UL_LSBLK_OWNER || column == UL_LSBLK_GROUP ||
               column == UL_LSBLK_MODE;
}

static bool ul_lsblk_column_needs_metadata(p8 column)
{
        return column == UL_LSBLK_TRAN || column == UL_LSBLK_VENDOR ||
               column == UL_LSBLK_MODEL || column == UL_LSBLK_REV ||
               column == UL_LSBLK_SERIAL || column == UL_LSBLK_HCTL;
}

static const file_long ul_lsblk_longs[] = {
    {"noempty", 'A'}, {"discard", 'D'}, {"json", 'J'},
    {"output-all", 'O'}, {"pairs", 'P'}, {"scsi", 'S'},
    {"all", 'a'}, {"bytes", 'b'}, {"nodeps", 'd'}, {"fs", 'f'},
    {"ascii", 'i'}, {"list", 'l'}, {"perms", 'm'},
    {"noheadings", 'n'}, {"output", 'o'}, {"paths", 'p'},
    {"raw", 'r'}, {"topology", 't'}, {"zoned", 'z'},
    {"sysroot", 's'}, {"help", 'h'}, {"version", 'V'}, {null, 0},
};

static b32 util_linux_lsblk()
{
        file_taking taking = {
            .program = "lsblk", .allowed = "ADJOPSabdfilmnoprtszVh",
            .valued = "os", .longs = ul_lsblk_longs,
        };
        b32 answer;
        if (!file_take(address_of taking))
                return 1;
        if (ul_meta(address_of taking, "[options] [device ...]",
                    address_of answer))
                return answer;
        if (taking.flags & (FILE_FLAG('D') | FILE_FLAG('z')))
                return ul_bad_usage(
                    "lsblk", "discard/zoned fields are not supported");
        if (taking.flags & (FILE_FLAG('O') | FILE_FLAG('P')))
                return ul_bad_usage(
                    "lsblk", "output-all/pairs metadata is not supported");
        if (file_option_value(address_of taking, 's'))
                return ul_bad_usage("lsblk", "--sysroot is not supported");

        positive presets = ((taking.flags & FILE_FLAG('f')) != 0) +
                           ((taking.flags & FILE_FLAG('m')) != 0) +
                           ((taking.flags & FILE_FLAG('t')) != 0) +
                           ((taking.flags & FILE_FLAG('S')) != 0);
        if (presets > 1)
                return ul_bad_usage("lsblk", "output presets are mutually exclusive");

        static const p8 defaults[] = {
            UL_LSBLK_NAME, UL_LSBLK_MAJMIN, UL_LSBLK_RM, UL_LSBLK_SIZE,
            UL_LSBLK_RO, UL_LSBLK_TYPE, UL_LSBLK_MOUNTPOINTS,
        };
        static const p8 fs_defaults[] = {
            UL_LSBLK_NAME, UL_LSBLK_FSTYPE, UL_LSBLK_FSVER, UL_LSBLK_LABEL,
            UL_LSBLK_UUID, UL_LSBLK_FSAVAIL, UL_LSBLK_FSUSE,
            UL_LSBLK_MOUNTPOINTS,
        };
        static const p8 perm_defaults[] = {
            UL_LSBLK_NAME, UL_LSBLK_SIZE, UL_LSBLK_OWNER,
            UL_LSBLK_GROUP, UL_LSBLK_MODE,
        };
        static const p8 topology_defaults[] = {
            UL_LSBLK_NAME, UL_LSBLK_ALIGNMENT, UL_LSBLK_MINIO,
            UL_LSBLK_OPTIO, UL_LSBLK_PHYSEC, UL_LSBLK_LOGSEC,
            UL_LSBLK_ROTA, UL_LSBLK_SCHED, UL_LSBLK_RQSIZE,
            UL_LSBLK_RA, UL_LSBLK_WSAME,
        };
        static const p8 scsi_defaults[] = {
            UL_LSBLK_NAME, UL_LSBLK_HCTL, UL_LSBLK_TYPE,
            UL_LSBLK_VENDOR, UL_LSBLK_MODEL, UL_LSBLK_REV,
            UL_LSBLK_TRAN,
        };
        const p8 address_to chosen =
            taking.flags & FILE_FLAG('f') ? fs_defaults :
            taking.flags & FILE_FLAG('m') ? perm_defaults :
            taking.flags & FILE_FLAG('t') ? topology_defaults :
            taking.flags & FILE_FLAG('S') ? scsi_defaults : defaults;
        positive chosen_count =
            taking.flags & FILE_FLAG('f') ? array_count(fs_defaults) :
            taking.flags & FILE_FLAG('m') ? array_count(perm_defaults) :
            taking.flags & FILE_FLAG('t') ? array_count(topology_defaults) :
            taking.flags & FILE_FLAG('S') ? array_count(scsi_defaults) :
                                             array_count(defaults);
        p8 columns[UL_LSBLK_COLUMNS];
        positive column_count = 0;
        string_address selected = file_option_value(address_of taking, 'o');
        if (selected &&
            !ul_table_column_list(selected, ul_lsblk_columns,
                                  UL_LSBLK_COLUMNS, chosen, chosen_count,
                                  columns, address_of column_count))
                return ul_bad_usage(
                    "lsblk", "unknown or unsupported output column");
        if (!selected)
                for (positive i = 0; i < chosen_count; i++)
                        columns[column_count++] = chosen[i];

        bool identity = false;
        bool permissions = false;
        bool metadata = false;
        for (positive i = 0; i < column_count; i++)
        {
                identity |= ul_lsblk_column_needs_identity(columns[i]);
                permissions |= ul_lsblk_column_needs_permissions(columns[i]);
                metadata |= ul_lsblk_column_needs_metadata(columns[i]);
        }
        bool scsi = (taking.flags & FILE_FLAG('S')) != 0;
        text_arena_used = 0;
        if (!ul_lsblk_take(identity, permissions, metadata))
                return ul_bad_usage("lsblk", "block-device sysfs is unavailable");
        if (!ul_lsblk_select_operands(taking.first))
                return 1;

        bool list = (taking.flags & FILE_FLAG('l')) != 0;
        bool raw = (taking.flags & FILE_FLAG('r')) != 0;
        bool dependencies = !(taking.flags & FILE_FLAG('d'));
        ul_lsblk_rows(list, dependencies,
                      (taking.flags & FILE_FLAG('a')) != 0,
                      (taking.flags & FILE_FLAG('A')) != 0, scsi);
        ul_lsblk_bytes = (taking.flags & FILE_FLAG('b')) != 0;
        ul_lsblk_tree = !list && !raw && dependencies;
        ul_lsblk_json = (taking.flags & FILE_FLAG('J')) != 0;
        ul_lsblk_paths = (taking.flags & FILE_FLAG('p')) != 0;
        if (ul_lsblk_json)
                ul_lsblk_json_out(columns, column_count);
        else
                ul_table_out(ul_lsblk.rows, sizeof(ul_lsblk.rows[0]),
                             ul_lsblk.row_count, ul_lsblk_columns,
                             UL_LSBLK_COLUMNS, columns, column_count,
                             !(taking.flags & FILE_FLAG('n')),
                             raw,
                             ul_lsblk_field);
        log_flush();
        return 0;
}
