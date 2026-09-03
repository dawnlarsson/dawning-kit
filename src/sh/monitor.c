/*
        The live monitor as one long-lived utility.

        The former shell workload launched grep, awk, sort, head, date, stty,
        cp and mv on every frame and serialized typed kernel counters through
        temporary text files between them.  This consumes the shared snapshot
        records directly, keeps the preceding sample in memory and writes one
        terminal transaction.  The two-line script remains only as the stable
        /monitor.sh interface.
*/

#define MONITOR_RESTORE "\033[?2026l\033[?25h\033[0m\033[?1049l"

typedef struct
{
        struct snapshot_process address_to process;
        positive tenths;
} monitor_top;

static positive monitor_row_left;
static volatile b32 monitor_stopping;

static HOT fn monitor_row_write(address_any data, positive length)
{
        if (length > monitor_row_left)
                length = monitor_row_left;
        if (length)
        {
                text_put(data, length);
                monitor_row_left -= length;
        }
}

static fn monitor_row_end(bool newline)
{
        text_put_string("\033[K");
        if (newline)
                text_put_character('\n');
}

static fn monitor_fill(positive count, p8 byte)
{
        p8 address_to room = text_reserve(count);

        if (room)
                memory_fill(room, byte, count);
}

/*
        A number scaled by ten or a hundred, the whole part in a field and
        the fraction after the point: the cpu tenths and the load hundredths
        are one shape with a different number of places. The point and the
        places count in the width, and a width of zero pads nothing.
*/
static fn monitor_fixed(writer write, positive value, positive places,
                        positive width)
{
        p8 fraction[3];

        fraction[0] = '.';

        if (places == 1)
                fraction[1] = (p8)('0' + value % 10);
        else
                positive_into_pair(fraction + 1, value % 100);

        positive_to_base_field(write, value / (places == 1 ? 10 : 100), 10,
                               width > places + 1 ? width - places - 1 : 0,
                               -1, 0);
        write(fraction, places + 1);
}

static CONST positive monitor_percent(positive part, positive whole,
                                      positive scale)
{
        if (!whole)
                return 0;
        if (part > positive_max / scale)
                return positive_max;

        positive scaled = part * scale;

        return scaled <= positive_max - whole / 2
                   ? (scaled + whole / 2) / whole
                   : scaled / whole;
}

// Swap in use and how full that is, which the layout asks before a frame
// is drawn and the memory rows ask again while drawing it.
static positive monitor_swap_percent(system_snapshot address_to sample,
                                     positive address_to used)
{
        positive total = sample->header.swap_total;
        positive taken = total >= sample->header.swap_free
                             ? total - sample->header.swap_free
                             : 0;

        if (used)
                address_to used = taken;

        return monitor_percent(taken, total, 100);
}

static fn monitor_bar(positive percent, positive width)
{
        positive filled = percent > 100 ? width : percent * width / 100;
        string_address colour = percent >= 85 ? "\033[31m"
                                : percent >= 60 ? "\033[33m"
                                                : "\033[32m";

        text_put_string("[");
        text_put_string(colour);
        monitor_fill(filled, '|');
        text_put_string("\033[0m");
        monitor_fill(width - filled, ' ');
        text_put_string("]");
}

static PURE struct snapshot_network address_to monitor_network_find(
    system_snapshot address_to sample, string_address name)
{
        for (positive i = 0; i < sample->header.network_count; i++)
                if (string_equals(sample->networks[i].name, name))
                        return sample->networks + i;

        return null;
}

static fn monitor_header(system_snapshot address_to sample,
                         string_address host, positive count,
                         positive columns)
{
        positive seconds = sample->header.uptime_ns / SYSTEM_NANOSECONDS;
        positive days = seconds / 86400;
        positive hours = seconds / 3600 % 24;
        positive minutes = seconds / 60 % 60;
        time_t now = (time_t)sample->header.realtime_seconds;
        tm broken;

        text_put_string("\033[1m ");
        writer_field(text_put, host, string_length(host), 14, ' ', true);
        text_put_string("\033[0m ");

        if (columns >= 72)
        {
                text_put_string("up ");

                if (days)
                {
                        positive_to_string(text_put, days);
                        text_put_character('d');
                        text_put_character(' ');
                }
                if (days || hours)
                {
                        positive_to_string(text_put, hours);
                        text_put_character('h');
                        text_put_character(' ');
                }
                positive_to_string(text_put, minutes);
                text_put_string("m ");

                text_put_string("load ");
                for (positive i = 0; i < 3; i++)
                {
                        monitor_fixed(text_put, sample->header.load[i], 2, 0);
                        text_put_character(' ');
                }
        }

        if (localtime_r(address_of now, address_of broken))
        {
                p8 clock[8];

                positive_into_padded(clock, (positive)broken.tm_hour, 2, '0');
                clock[2] = ':';
                positive_into_padded(clock + 3, (positive)broken.tm_min, 2, '0');
                clock[5] = ':';
                positive_into_padded(clock + 6, (positive)broken.tm_sec, 2, '0');
                text_put(clock, sizeof(clock));
        }

        text_put_string(" #");
        positive_to_padded(text_put, count % 100, 2, '0', 0);
        text_put_string("\033[K\n\033[K\n");
}

static fn monitor_cpus(system_snapshot address_to old,
                       system_snapshot address_to sample, positive rows,
                       positive bar_width)
{
        positive shown = 0;
        positive previous = 0;

        for (positive i = 0; i < sample->header.cpu_count && shown < rows; i++)
        {
                struct snapshot_cpu address_to cpu = sample->cpus + i;
                unsigned int id = cpu->id;

                while (previous < old->header.cpu_count &&
                       old->cpus[previous].id != id &&
                       (old->cpus[previous].id == ~0u ||
                        old->cpus[previous].id < id))
                        previous++;

                struct snapshot_cpu address_to before =
                    previous < old->header.cpu_count &&
                            old->cpus[previous].id == id
                        ? old->cpus + previous++
                        : null;
                positive total = before && cpu->total_ns >= before->total_ns
                                     ? cpu->total_ns - before->total_ns
                                     : 0;
                positive idle = before && cpu->idle_ns >= before->idle_ns
                                    ? cpu->idle_ns - before->idle_ns
                                    : 0;
                positive busy = total >= idle ? total - idle : 0;
                positive tenths = monitor_percent(busy, total, 1000);
                p8 name[24];
                positive name_length;

                if (cpu->id == ~0u)
                {
                        memory_copy_apart(name, "all", 3);
                        name_length = 3;
                }
                else
                {
                        memory_copy_apart(name, "cpu", 3);
                        name_length = 3 + positive_into(name + 3, cpu->id);
                }

                text_put_character(' ');
                writer_field(text_put, name, name_length, 6, ' ', true);
                text_put_string(" ");
                monitor_bar((tenths + 5) / 10, bar_width);
                text_put_character(' ');
                monitor_fixed(text_put, tenths, 1, 5);
                text_put_string("%\033[K\n");
                shown++;
        }
}

static fn monitor_memory(system_snapshot address_to sample,
                         positive bar_width)
{
        positive total = sample->header.memory_total;
        positive available = sample->header.memory_available;
        positive used = total >= available ? total - available : 0;
        positive percent = monitor_percent(used, total, 100);
        p8 used_text[9];
        p8 total_text[9];
        positive used_length = positive_into_human_nearest_string(
            used_text, used, true);
        positive total_length = positive_into_human_nearest_string(
            total_text, total, true);

        text_put_string(" mem    ");
        monitor_bar(percent, bar_width);
        text_put_character(' ');
        positive_to_base_field(text_put, percent, 10, 3, -1, 0);
        text_put_string("%  ");
        text_put(used_text, used_length);
        text_put_string(" / ");
        text_put(total_text, total_length);
        text_put_string("\033[K\n");

        positive swap_used;
        positive swap_percent = monitor_swap_percent(sample,
                                                     address_of swap_used);

        if (swap_percent)
        {
                p8 swap_text[9];
                positive swap_length = positive_into_human_nearest_string(
                    swap_text, swap_used, true);

                text_put_string(" swap   ");
                monitor_bar(swap_percent, bar_width);
                text_put_character(' ');
                positive_to_base_field(text_put, swap_percent, 10, 3, -1, 0);
                text_put_string("%  ");
                text_put(swap_text, swap_length);
                text_put_string("\033[K\n");
        }
}

static fn monitor_networks(system_snapshot address_to old,
                           system_snapshot address_to sample, positive rows,
                           positive columns, positive elapsed_ns)
{
        positive shown = 0;
        positive elapsed_us = elapsed_ns / 1000;

        if (!elapsed_us)
                elapsed_us = 1;

        for (positive i = 0; i < sample->header.network_count && shown < rows;
             i++)
        {
                struct snapshot_network address_to network =
                    sample->networks + i;

                if (string_equals(network->name, "lo"))
                        continue;

                struct snapshot_network address_to before =
                    monitor_network_find(old, network->name);
                positive received = before &&
                                            network->received >= before->received
                                        ? network->received - before->received
                                        : 0;
                positive transmitted =
                    before && network->transmitted >= before->transmitted
                        ? network->transmitted - before->transmitted
                        : 0;

                if (!received && !transmitted && !network->received)
                        continue;

                positive down = received > positive_max / 1000000
                                    ? positive_max
                                    : received * 1000000 / elapsed_us;
                positive up = transmitted > positive_max / 1000000
                                  ? positive_max
                                  : transmitted * 1000000 / elapsed_us;
                p8 down_text[16];
                p8 up_text[16];
                positive down_length =
                    positive_into_human_nearest_string(down_text, down, false);
                positive up_length =
                    positive_into_human_nearest_string(up_text, up, false);
                monitor_row_left = columns > 1 ? columns - 1 : 1;
                monitor_row_write(" ", 1);
                string_to_field(monitor_row_write, network->name, 10, ' ',
                                true);
                monitor_row_write(" down ", 6);
                down_text[down_length++] = '/';
                down_text[down_length++] = 's';
                writer_field(monitor_row_write, down_text, down_length, 12,
                             ' ', true);
                monitor_row_write(" up ", 4);
                up_text[up_length++] = '/';
                up_text[up_length++] = 's';
                writer_field(monitor_row_write, up_text, up_length, 12, ' ',
                             true);
                monitor_row_end(true);
                shown++;
        }
}

static HOT fn monitor_top_insert(monitor_top address_to top, positive rows,
                                 positive address_to count,
                                 struct snapshot_process address_to process,
                                 positive tenths)
{
        positive at = 0;

        while (at < address_to count && top[at].tenths >= tenths)
                at++;

        if (at >= rows)
                return;

        positive stop = address_to count < rows ? address_to count
                                                 : rows - 1;

        while (stop > at)
        {
                top[stop] = top[stop - 1];
                stop--;
        }

        top[at].process = process;
        top[at].tenths = tenths;
        if (address_to count < rows)
                address_to count += 1;
}

static bool monitor_processes(system_snapshot address_to old,
                              system_snapshot address_to sample,
                              monitor_top address_to address_to top,
                              positive address_to top_room, positive rows,
                              positive columns, positive elapsed_ns)
{
        if (!memory_reserve((address_any address_to)top, top_room, 0, rows,
                            sizeof(monitor_top), 16))
                return false;

        positive count = 0;
        positive before = 0;

        for (positive i = 0; i < sample->header.process_count; i++)
        {
                struct snapshot_process address_to process =
                    sample->processes + i;

                while (before < old->header.process_count &&
                       old->processes[before].pid < process->pid)
                        before++;

                positive used = 0;

                if (before < old->header.process_count &&
                    old->processes[before].pid == process->pid)
                {
                        positive now = system_saturating_add(
                            process->user_ns, process->system_ns);
                        positive was = system_saturating_add(
                            old->processes[before].user_ns,
                            old->processes[before].system_ns);
                        used = now >= was ? now - was : 0;
                }

                positive tenths = monitor_percent(used, elapsed_ns, 1000);

                monitor_top_insert(address_to top, rows, address_of count,
                                   process, tenths);
        }

        text_put_string("\033[K\n\033[1m pid       cpu%    memory  command\033[0m\033[K\n");

        for (positive i = 0; i < count; i++)
        {
                p8 memory_text[9];
                positive memory_length = positive_into_human_nearest_string(
                    memory_text, (address_to top)[i].process->resident_bytes,
                    true);
                monitor_row_left = columns > 1 ? columns - 1 : 1;
                monitor_row_write(" ", 1);
                positive_to_base_field(
                    monitor_row_write, (address_to top)[i].process->pid, 10,
                    7, -1, (positive)1 << 27);
                monitor_row_write(" ", 1);
                monitor_fixed(monitor_row_write, (address_to top)[i].tenths, 1,
                              6);
                monitor_row_write(" ", 1);
                writer_field(monitor_row_write, memory_text, memory_length, 9,
                             ' ', false);
                monitor_row_write("  ", 2);
                monitor_row_write((address_to top)[i].process->command,
                                  string_length(
                                      (address_to top)[i].process->command));
                monitor_row_end(i + 1 < count);
        }

        return true;
}

static fn monitor_caught(b32 number)
{
        (void)number;
        monitor_stopping = 1;
}

#define MONITOR_SIGNAL_BLOCK 0
#define MONITOR_SIGNAL_SET_MASK 2
#define MONITOR_EINTR 4

/*
        The interval, cut short by a signal: one when it slept, zero when
        it was told to stop, negative when the kernel refused.

        The three signals that stop the monitor are blocked while the flag
        is read and let through only inside ppoll, which swaps the mask in
        and sleeps as one step. A signal landing between the test and the
        call is then delivered inside the call and ends it, where nanosleep
        after the same test slept the whole interval with the flag already
        set. ppoll with nothing to poll is the sleep every architecture
        has, and the last argument is the size of a signal set, which the
        kernel checks.
*/
static b32 monitor_sleep(p64 address_to span)
{
        positive stopping = ((positive)1 << 0) | ((positive)1 << 1) |
                            ((positive)1 << 14);
        positive previous = 0;
        b32 answer = -1;

        if (system_signal_mask(MONITOR_SIGNAL_BLOCK, address_of stopping,
                               address_of previous, 8) < 0)
                return -1;

        while (1)
        {
                if (monitor_stopping)
                {
                        answer = 0;
                        break;
                }

                bipolar polled = system_call_5(syscall(ppoll), 0, 0,
                                               (positive)span,
                                               (positive)address_of previous,
                                               8);

                if (polled >= 0)
                {
                        answer = 1;
                        break;
                }

                // Some other signal cut the sleep short, and the interval
                // starts over.
                if (polled != -MONITOR_EINTR)
                        break;
        }

        system_signal_mask(MONITOR_SIGNAL_SET_MASK, address_of previous, 0, 8);

        return answer;
}

static HOT b32 tools_monitor()
{
        positive arguments = (positive)program_argument_count();
        p64 interval[2] = {0, 500000000};
        positive frames = 0;

        if (arguments > 3 ||
            (arguments > 1 &&
             (!sleep_read(program_argument(1), address_of interval[0],
                          address_of interval[1]) ||
              (!interval[0] && !interval[1]))) ||
            (arguments > 2 &&
             !string_digits_exact(program_argument(2), address_of frames)))
        {
                text_error(null, "usage: monitor [interval] [frames]");
                return text_done(2);
        }

        file_machine facts;
        p8 host[15];

        memory_fill(address_of facts, 0, sizeof(facts));
        memory_fill(host, 0, sizeof(host));
        if (system_call_1(syscall(uname), (positive)address_of facts) >= 0)
        {
                positive length = string_length_max(facts.node, 14);

                memory_copy_apart(host, facts.node, length);
        }

        static system_snapshot samples[2];
        system_snapshot address_to old = samples;
        system_snapshot address_to sample = old;
        static monitor_top address_to top;
        static positive top_room;
        positive count = 0;
        b32 status = 0;

        monitor_stopping = 0;
        system_signal_install(1, (positive)monitor_caught, SIGNAL_CATCH_FLAGS,
                              SIGNAL_CATCH_RESTORER, null);
        system_signal_install(2, (positive)monitor_caught, SIGNAL_CATCH_FLAGS,
                              SIGNAL_CATCH_RESTORER, null);
        system_signal_install(15, (positive)monitor_caught, SIGNAL_CATCH_FLAGS,
                              SIGNAL_CATCH_RESTORER, null);
        text_put_string("\033[?1049h\033[?25l\033[2J\033[H\033[1m monitor\033[0m  sampling...\033[K");
        text_flush();

        if (!system_snapshot_take(old, SPARK_SNAPSHOT_ALL, false))
        {
                text_error("/proc", "cannot read system snapshot");
                status = 1;
                goto finished;
        }

        while (!monitor_stopping)
        {
                if (count)
                {
                        b32 slept = monitor_sleep(interval);

                        if (slept <= 0)
                        {
                                if (slept < 0)
                                {
                                        text_error(null, "monitor: sleep failed");
                                        status = 1;
                                }
                                break;
                        }

                        sample = old == samples ? samples + 1 : samples;

                        if (!system_snapshot_take(sample, SPARK_SNAPSHOT_ALL,
                                                  false))
                        {
                                text_error("/proc", "cannot read system snapshot");
                                status = 1;
                                break;
                        }
                }

                positive elapsed_ns = sample->header.monotonic_ns >=
                                               old->header.monotonic_ns
                                          ? sample->header.monotonic_ns -
                                                old->header.monotonic_ns
                                          : 0;

                if (!elapsed_ns)
                        elapsed_ns = 10000000;

                positive2 size = term_size();
                positive columns = size.width ? size.width : 80;
                positive rows = size.height ? size.height : 24;

                text_put_string("\033[?2026h\033[H");

                if (rows < 12 || columns < 40)
                {
                        text_put_string(" monitor ");
                        positive_to_string(text_put, columns);
                        text_put_character('x');
                        positive_to_string(text_put, rows);
                        text_put_string("\033[K\033[J");
                }
                else
                {
                        positive networks = 0;

                        for (positive i = 0;
                             i < sample->header.network_count; i++)
                                networks += !string_equals(
                                    sample->networks[i].name, "lo");

                        positive swap = monitor_swap_percent(sample, null) != 0;
                        positive network_room = rows > 7 + swap
                                                    ? rows - 7 - swap
                                                    : 0;

                        if (networks > network_room)
                                networks = network_room;

                        positive fixed = 5 + swap + networks;
                        positive available = rows > fixed ? rows - fixed : 2;

                        if (available < 2)
                                available = 2;

                        positive cpu_rows = available / 2;

                        if (cpu_rows > sample->header.cpu_count)
                                cpu_rows = sample->header.cpu_count;
                        if (!cpu_rows)
                                cpu_rows = 1;

                        positive process_rows = available - cpu_rows;

                        if (!process_rows)
                                process_rows = 1;

                        monitor_header(sample, host, count, columns);
                        monitor_cpus(old, sample, cpu_rows, columns - 18);
                        monitor_memory(sample, columns - 39);
                        monitor_networks(old, sample, networks, columns,
                                         elapsed_ns);

                        if (!monitor_processes(old, sample, address_of top,
                                               address_of top_room,
                                               process_rows, columns,
                                               elapsed_ns))
                        {
                                status = 1;
                                break;
                        }

                        text_put_string("\033[J");
                }

                text_put_string("\033[?2026l");
                text_flush();
                count++;

                if (frames && count >= frames)
                        break;

                old = sample;
        }

finished:
        text_put_string(MONITOR_RESTORE);
        return text_done(status);
}

#undef MONITOR_RESTORE
#undef MONITOR_SIGNAL_BLOCK
#undef MONITOR_SIGNAL_SET_MASK
#undef MONITOR_EINTR
