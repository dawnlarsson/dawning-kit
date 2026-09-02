/*
        A reusable view of Linux's live system state.

        Utilities ask for sections and receive typed records in caller-owned,
        reusable storage.  procfs is parsed once here instead of independently
        by ps, dashboards and every future status tool.  The structures are
        deliberately the /dev/spark snapshot ABI: an accelerated kernel can
        fill them directly without changing a consumer.
*/

#define SYSTEM_NANOSECONDS 1000000000u
#define SYSTEM_USER_HZ 100u
#define SYSTEM_TICK_NS (SYSTEM_NANOSECONDS / SYSTEM_USER_HZ)

typedef struct
{
        struct snapshot_header header;
        struct snapshot_cpu address_to cpus;
        positive cpu_room;
        struct snapshot_network address_to networks;
        positive network_room;
        struct snapshot_process address_to processes;
        positive process_room;
        struct snapshot_process address_to process_spare;
        positive process_spare_room;
        p8 address_to kernel;
        positive kernel_room;
        byte_store input;
} system_snapshot;

/* Linux's 64-bit sysinfo ABI. Keeping this private avoids importing libc's
   headers into the freestanding shell. */
typedef struct
{
        bipolar uptime;
        positive loads[3];
        positive totalram;
        positive freeram;
        positive sharedram;
        positive bufferram;
        positive totalswap;
        positive freeswap;
        p16 processes;
        p16 padding;
        positive totalhigh;
        positive freehigh;
        p32 memory_unit;
} system_information;

_Static_assert(sizeof(system_information) == 112, "Linux sysinfo ABI");

static CONST bool system_snapshot_range(unsigned int offset,
                                        unsigned int count, positive width,
                                        unsigned int used)
{
        return offset <= used && count <= positive_max / width &&
               count * width <= used - offset;
}

static HOT bool system_snapshot_accelerated(system_snapshot address_to sample,
                                            unsigned int flags)
{
        static bipolar device = -2;

        if (device == -2)
                device = system_open_at(AT_FDCWD, SPARK_DEVICE,
                                        FILE_READ | O_CLOEXEC);
        if (device < 0)
                return false;

        if (!sample->kernel_room &&
            !memory_reserve((address_any address_to)address_of sample->kernel,
                            address_of sample->kernel_room, 0, 4096, 1, 4096))
                return false;

        struct snapshot_request request = {
            .buffer = (unsigned long)sample->kernel,
            .capacity = (unsigned int)sample->kernel_room,
            .flags = flags,
            .version = SPARK_SNAPSHOT_VERSION,
        };
        bipolar answer = system_control(device, SPARK_IOCTL_SNAPSHOT,
                                        address_of request);

        if (answer == -28 && request.required > request.capacity &&
            request.required <= SPARK_SNAPSHOT_MAX_BYTES &&
            memory_reserve((address_any address_to)address_of sample->kernel,
                           address_of sample->kernel_room, 0,
                           request.required, 1, 4096))
        {
                request.buffer = (unsigned long)sample->kernel;
                request.capacity = (unsigned int)sample->kernel_room;
                answer = system_control(device, SPARK_IOCTL_SNAPSHOT,
                                        address_of request);
        }

        if (answer < 0 || request.used < sizeof(struct snapshot_header) ||
            request.used > sample->kernel_room)
        {
                system_close(device);
                device = -1;
                return false;
        }

        struct snapshot_header address_to header =
            (struct snapshot_header address_to)sample->kernel;

        if (header->version != SPARK_SNAPSHOT_VERSION ||
            header->bytes != request.used || header->flags != flags ||
            header->page_size < 1024 ||
            ((flags & SPARK_SNAPSHOT_CPU) &&
             !system_snapshot_range(header->cpu_offset, header->cpu_count,
                                    sizeof(struct snapshot_cpu),
                                    request.used)) ||
            ((flags & SPARK_SNAPSHOT_NETWORK) &&
             !system_snapshot_range(header->network_offset,
                                    header->network_count,
                                    sizeof(struct snapshot_network),
                                    request.used)))
        {
                system_close(device);
                device = -1;
                return false;
        }

        if ((flags & SPARK_SNAPSHOT_CPU) &&
            !memory_reserve(
                 (address_any address_to)address_of sample->cpus,
                 address_of sample->cpu_room, 0, header->cpu_count,
                 sizeof(struct snapshot_cpu), 16))
                return false;

        if ((flags & SPARK_SNAPSHOT_NETWORK) &&
            !memory_reserve(
                 (address_any address_to)address_of sample->networks,
                 address_of sample->network_room, 0, header->network_count,
                 sizeof(struct snapshot_network), 8))
                return false;

        sample->header = *header;

        if (flags & SPARK_SNAPSHOT_CPU)
                memory_copy_apart(sample->cpus,
                                  sample->kernel + header->cpu_offset,
                                  header->cpu_count *
                                      sizeof(struct snapshot_cpu));
        if (flags & SPARK_SNAPSHOT_NETWORK)
        {
                memory_copy_apart(sample->networks,
                                  sample->kernel + header->network_offset,
                                  header->network_count *
                                      sizeof(struct snapshot_network));
                for (positive i = 0; i < header->network_count; i++)
                        sample->networks[i].name[15] = end;
        }

        return true;
}

static CONST positive system_scaled(positive value, positive scale)
{
        return value > positive_max / scale ? positive_max : value * scale;
}

static PURE string_address system_field_start(string_address at)
{
        while (string_get(at) == ' ' || string_get(at) == '\t')
                at++;

        return at;
}

static positive system_field_unsigned(string_address address_to at)
{
        string_address here = system_field_start(address_to at);
        positive used = 0;
        positive value = string_digits(here, address_of used);

        address_to at = here + used;
        return value;
}

static bipolar system_field_signed(string_address address_to at)
{
        string_address here = system_field_start(address_to at);
        positive used = 0;
        bipolar value = string_bipolar(here, address_of used);

        address_to at = here + used;
        return value;
}

static fn system_fields_pass(string_address address_to at, positive count)
{
        string_address here = address_to at;

        while (count--)
        {
                here = system_field_start(here);

                while (string_get(here) && string_get(here) != ' ' &&
                       string_get(here) != '\t' && string_get(here) != '\n')
                        here++;
        }

        address_to at = here;
}

static positive system_clock_ns(b32 clock)
{
        p64 moment[2] = {0, 0};

        return system_call_2(syscall(clock_gettime), (positive)clock,
                             (positive)moment) < 0
                   ? 0
                   : moment[0] * SYSTEM_NANOSECONDS + moment[1];
}

static positive system_page_size()
{
        static positive page;

        if (!page)
        {
                p8 auxv[512];
                bipolar got = file_slurp_once_at(AT_FDCWD, "/proc/self/auxv",
                                                  auxv, sizeof(auxv));
                positive pair = 2 * sizeof(positive);

                if (got > 0)
                        for (positive at = 0; at + pair <= (positive)got;
                             at += pair)
                        {
                                positive type;
                                positive value;

                                memory_copy_apart(address_of type, auxv + at,
                                                  sizeof(positive));
                                memory_copy_apart(address_of value,
                                                  auxv + at + sizeof(positive),
                                                  sizeof(positive));

                                if (type == 6 && value >= 1024)
                                {
                                        page = value;
                                        break;
                                }
                        }

                if (!page)
                        page = 4096;
        }

        return page;
}

static HOT bool system_snapshot_system(system_snapshot address_to sample)
{
        system_information information;

        sample->header.monotonic_ns = system_clock_ns(1);
        sample->header.uptime_ns = system_clock_ns(7);
        sample->header.realtime_seconds = system_clock_ns(0) /
                                           SYSTEM_NANOSECONDS;
        sample->header.page_size = (unsigned int)system_page_size();

        if (system_call_1(syscall(sysinfo),
                          (positive)address_of information) < 0)
                return false;

        for (positive i = 0; i < 3; i++)
        {
                positive whole = information.loads[i] >> 16;
                positive fraction = information.loads[i] & 0xffff;

                sample->header.load[i] = (unsigned int)(
                    system_scaled(whole, 100) +
                    (fraction * 100 + 0x8000) / 0x10000);
        }

        positive unit = information.memory_unit ? information.memory_unit : 1;

        sample->header.memory_total = system_scaled(information.totalram, unit);
        sample->header.memory_available = system_scaled(information.freeram,
                                                         unit);
        sample->header.swap_total = system_scaled(information.totalswap, unit);
        sample->header.swap_free = system_scaled(information.freeswap, unit);

        if (!file_store_slurp("/proc/meminfo", address_of sample->input))
                return false;

        string_address at = sample->input.bytes;
        while (string_get(at))
        {
                string_address next = string_first_of_or_end(at, '\n');

                if (!string_compare_max(at, "MemAvailable:", 13))
                {
                        string_address value_at = string_first_of_or_end(at, ':');

                        if (string_get(value_at))
                        {
                                value_at++;
                                sample->header.memory_available = system_scaled(
                                    system_field_unsigned(address_of value_at),
                                    1024);
                        }
                        break;
                }

                at = string_get(next) ? next + 1 : next;
        }

        return true;
}

static HOT bool system_snapshot_cpu(system_snapshot address_to sample)
{
        if (!file_store_slurp("/proc/stat", address_of sample->input))
                return false;

        string_address line = sample->input.bytes;

        while (line[0] == 'c' && line[1] == 'p' && line[2] == 'u' &&
               (line[3] == ' ' || byte_is_digit(line[3])))
        {
                if (!memory_reserve(
                        (address_any address_to)address_of sample->cpus,
                        address_of sample->cpu_room, sample->header.cpu_count,
                        sample->header.cpu_count + 1,
                        sizeof(struct snapshot_cpu), 16))
                        return false;

                struct snapshot_cpu address_to cpu =
                    sample->cpus + sample->header.cpu_count++;
                string_address at = line + 3;
                positive total = 0;
                positive idle = 0;

                memory_fill(cpu, 0, sizeof(*cpu));
                cpu->id = line[3] == ' '
                              ? ~0u
                              : (unsigned int)system_field_unsigned(address_of at);

                /* /proc/stat's guest fields are already in user/nice. */
                for (positive field = 0; field < 8; field++)
                {
                        positive value = system_field_unsigned(address_of at);

                        if (total <= positive_max - value)
                                total += value;
                        else
                                total = positive_max;

                        if (field == 3 || field == 4)
                                idle = idle <= positive_max - value
                                           ? idle + value
                                           : positive_max;
                }

                cpu->total_ns = system_scaled(total, SYSTEM_TICK_NS);
                cpu->idle_ns = system_scaled(idle, SYSTEM_TICK_NS);
                line = string_first_of_or_end(line, '\n');
                line += string_get(line) != end;
        }

        return sample->header.cpu_count != 0;
}

static HOT bool system_snapshot_network(system_snapshot address_to sample)
{
        if (!file_store_slurp("/proc/net/dev", address_of sample->input))
                return false;

        string_address line = sample->input.bytes;

        while (string_get(line))
        {
                string_address next = string_first_of_or_end(line, '\n');
                string_address colon = line;

                while (colon < next && string_get(colon) != ':')
                        colon++;

                if (colon < next)
                {
                        string_address name = line;
                        string_address name_end = colon;

                        while (name < name_end && (*name == ' ' || *name == '\t'))
                                name++;
                        while (name_end > name &&
                               (name_end[-1] == ' ' || name_end[-1] == '\t'))
                                name_end--;

                        positive length = (positive)(name_end - name);

                        if (length && length < 16)
                        {
                                if (!memory_reserve(
                                        (address_any address_to)address_of
                                            sample->networks,
                                        address_of sample->network_room,
                                        sample->header.network_count,
                                        sample->header.network_count + 1,
                                        sizeof(struct snapshot_network), 8))
                                        return false;

                                struct snapshot_network address_to network =
                                    sample->networks +
                                    sample->header.network_count++;
                                string_address at = colon + 1;

                                memory_fill(network, 0, sizeof(*network));
                                memory_copy_apart(network->name, name, length);
                                network->received =
                                    system_field_unsigned(address_of at);
                                system_fields_pass(address_of at, 7);
                                network->transmitted =
                                    system_field_unsigned(address_of at);
                        }
                }

                line = string_get(next) ? next + 1 : next;
        }

        return true;
}

static HOT bool system_process_parse(
    string_address text, positive page_size,
    struct snapshot_process address_to process)
{
        string_address open = string_first_of_or_end(text, '(');
        string_address close = string_last_of(text, ')');

        if (!string_get(open) || !close || close <= open)
                return false;

        memory_fill(process, 0, sizeof(*process));
        process->pid = (unsigned int)system_field_unsigned(address_of text);

        positive command = (positive)(close - open - 1);
        if (command > sizeof(process->command) - 1)
                command = sizeof(process->command) - 1;
        memory_copy_apart(process->command, open + 1, command);

        string_address at = close + 2;
        process->state = (unsigned int)string_get(at);
        at += string_get(at) != end;
        process->ppid = (unsigned int)system_field_unsigned(address_of at);
        process->pgrp = (unsigned int)system_field_unsigned(address_of at);
        process->session = (unsigned int)system_field_unsigned(address_of at);
        process->tty = (int)system_field_signed(address_of at);
        process->tpgid = (int)system_field_signed(address_of at);
        system_fields_pass(address_of at, 5);
        process->user_ns = system_scaled(system_field_unsigned(address_of at),
                                         SYSTEM_TICK_NS);
        process->system_ns = system_scaled(system_field_unsigned(address_of at),
                                           SYSTEM_TICK_NS);
        system_fields_pass(address_of at, 3);
        process->nice = (int)system_field_signed(address_of at);
        process->threads =
            (unsigned int)system_field_unsigned(address_of at);
        system_fields_pass(address_of at, 1);
        process->start_ns = system_scaled(system_field_unsigned(address_of at),
                                          SYSTEM_TICK_NS);
        process->virtual_bytes = system_field_unsigned(address_of at);
        process->resident_bytes = system_scaled(
            system_field_unsigned(address_of at), page_size);
        return process->pid != 0;
}

#define system_process_order(left, right) ((left).pid <= (right).pid ? -1 : 1)

static HOT bool system_snapshot_processes(system_snapshot address_to sample)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, "/proc"))
                return false;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (!byte_is_digit(entry->d_name[0]))
                        continue;

                p8 path[64];
                p8 block[8192];
                path_join(path, sizeof(path), entry->d_name, "stat");

                if (file_slurp_once_at(walk.handle, path, block,
                                       sizeof(block)) <= 0)
                        continue;

                if (!memory_reserve(
                        (address_any address_to)address_of sample->processes,
                        address_of sample->process_room,
                        sample->header.process_count,
                        sample->header.process_count + 1,
                        sizeof(struct snapshot_process), 128))
                {
                        file_walk_close(address_of walk);
                        return false;
                }

                if (system_process_parse(
                        block, sample->header.page_size,
                        sample->processes + sample->header.process_count))
                        sample->header.process_count++;
        }

        file_walk_close(address_of walk);

        positive count = sample->header.process_count;

        if (count > 1)
        {
                if (!memory_reserve(
                        (address_any address_to)address_of sample->process_spare,
                        address_of sample->process_spare_room, 0, count,
                        sizeof(struct snapshot_process), 128))
                        return false;

                struct snapshot_process address_to ordered = array_merge_sort(
                    sample->processes, sample->process_spare, count,
                    system_process_order);

                if (ordered != sample->processes)
                        memory_copy_apart(sample->processes, ordered,
                                          count * sizeof(*ordered));
        }

        return true;
}

#undef system_process_order

static HOT bool system_snapshot_take(system_snapshot address_to sample,
                                     unsigned int flags)
{
        memory_fill(address_of sample->header, 0, sizeof(sample->header));
        sample->header.version = SPARK_SNAPSHOT_VERSION;
        sample->header.flags = flags;
        sample->header.page_size = (unsigned int)system_page_size();

        unsigned int kernel_flags =
            flags & (SPARK_SNAPSHOT_SYSTEM | SPARK_SNAPSHOT_CPU |
                     SPARK_SNAPSHOT_NETWORK);
        bool accelerated = kernel_flags &&
                           system_snapshot_accelerated(sample, kernel_flags);

        if (!accelerated && (flags & SPARK_SNAPSHOT_SYSTEM) &&
            !system_snapshot_system(sample))
                return false;
        if (!accelerated && (flags & SPARK_SNAPSHOT_CPU) &&
            !system_snapshot_cpu(sample))
                return false;
        if (!accelerated && (flags & SPARK_SNAPSHOT_NETWORK) &&
            !system_snapshot_network(sample))
                return false;
        if ((flags & SPARK_SNAPSHOT_PROCESS) &&
            !system_snapshot_processes(sample))
                return false;

        sample->header.flags = flags;
        return true;
}
