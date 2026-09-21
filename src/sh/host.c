/*
        Moonwater on a disk.

        The system is one EFI image -- the kernel with the userspace built into
        it -- so installing Moonwater copies no root filesystem. It lays a GPT
        on a disk with two partitions: a FAT32 system partition holding that
        image where firmware looks for one, and an ext4 holding what a live
        session loses at power off, the bowl roots, /root and /home. Updating
        is copying a newer image over the old one; the data stays where it is.

        Which image is which comes from the image. The x86 boot header carries
        the kernel's version string -- release, builder, and the count and date
        of its link, the words uname and /proc/version give back -- so a
        running system finds its own image on the stick it started from by
        reading headers, and tells an installed disk carrying this very build
        from one carrying another.

        `moonwater boot` is a service of init's, and settles which of three a
        machine is before any shell starts:

          live  nothing installed: nothing is kept, as before
          disk  an install carrying this build: its data is mounted over the
                bowl roots, /root and /home
          ask   an install carrying another build, which is what a stick with
                a newer Moonwater looks like to an installed machine

        PID 1 cannot ask anything. The canvas console only prints, and the
        keyboard belongs to the terminal the compositor starts, so the verdict
        is written under /run/moonwater and that terminal asks before its
        shell does: use the disk's data with this build, update the disk to
        this build first, or leave the disk alone. `moonwater use`, `update`
        and `live` answer the same question from any shell.
*/

#define host_label TERM_BOLD "[Moonwater]" TERM_RESET " "

#define HOST_STATE "/run/moonwater"
#define HOST_VERDICT HOST_STATE "/verdict"
#define HOST_VERDICT_NEXT HOST_STATE "/verdict.next"
#define HOST_QUESTION HOST_STATE "/question"
#define HOST_QUESTION_TAKEN HOST_STATE "/question.taken"
#define HOST_HINT HOST_STATE "/hint"
#define HOST_HINT_TAKEN HOST_STATE "/hint.taken"
#define HOST_DATA HOST_STATE "/data"
#define HOST_SYSTEM HOST_STATE "/system"
#define HOST_MEDIUM HOST_STATE "/medium"
#define HOST_LOOK HOST_STATE "/look"
#define HOST_MACHINE_SCRIPT "/root/main.moonwater.sh"
#define HOST_MACHINE_BUILTIN "builtin"
#define HOST_MACHINE_RUNTIME HOST_STATE "/machine.sh"
#define HOST_MACHINE_DIRTY HOST_STATE "/machine.dirty"

#include "../moonwater.c"

#define HOST_SYSTEM_NAME "moonwater-boot"
#define HOST_DATA_NAME "moonwater-data"
#define HOST_IMAGE "/EFI/BOOT/BOOTX64.EFI"
#define HOST_IMAGE_NEXT "/EFI/BOOT/BOOTX64.NEW"
#define HOST_KEPT_DIRECTORY "/EFI/moonwater"
#define HOST_KEPT_IMAGE HOST_KEPT_DIRECTORY "/previous.efi"
#define HOST_KEPT_IMAGE_NEXT HOST_KEPT_DIRECTORY "/previous.new"

#define HOST_NAME_ROOM 64
#define HOST_PATH_ROOM 256
#define HOST_BUILD_ROOM 256
#define HOST_INSTALLS 8
#define HOST_SYSTEM_BYTES ((p64)512 << 20)
#define HOST_ALIGN_BYTES ((p64)1 << 20)
#define HOST_SMALLEST ((p64)2 << 30)
#define HOST_READ_ONLY (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC)
#define HOST_WRITABLE (MS_NOSUID | MS_NODEV | MS_NOEXEC)

/*
        How long boot looks for disks. NVMe namespaces and USB disks arrive
        after init starts -- usb-storage waits a second before it scans -- so
        a first look finds nothing on a machine that has everything. Boot
        stops as soon as an install turns up, and otherwise once a second has
        passed and nothing is still on its way, and never later than ten.
*/
#define HOST_SETTLE_FLOOR_NS ((p64)1000000000)
#define HOST_SETTLE_MOST_NS ((p64)10000000000)
#define HOST_POLL_NS ((p64)100000000)
#define HOST_VERDICT_WAIT_NS ((p64)15000000000)
#define HOST_LOOKING_NS ((p64)700000000)
#define HOST_MEDIUM_FLOOR_NS ((p64)5000000000)

#define HOST_CLOCK_REALTIME 0
#define HOST_CLOCK_BOOTTIME 7
#define HOST_BLKRRPART 0x125f
#define HOST_BLKSSZGET 0x1268
#define HOST_BLKGETSIZE64 0x80081272u

/* What an install keeps, and each directory's mode where it is made new. */
static const struct
{
        string_address path;
        positive mode;
} host_kept[] = {
    {BOWL_ROOT_DIRECTORY, 0755},
    {"/root", 0700},
    {"/home", 0755},
};

typedef struct
{
        p8 disk[HOST_NAME_ROOM];
        p8 system[HOST_NAME_ROOM];
        p8 data[HOST_NAME_ROOM];
        p8 system_partuuid[STORAGE_PARTUUID_ROOM];
        p8 data_partuuid[STORAGE_PARTUUID_ROOM];
        p8 build[HOST_BUILD_ROOM];
        bool readable;
} host_install;

typedef struct
{
        host_install found[HOST_INSTALLS];
        positive count;
} host_census;

// Settings, defined further down with the commands that change them.
typedef struct spark_settings host_settings;

static fn host_settings_empty(host_settings address_to settings);
static b32 host_settings_image(string_address path, host_settings address_to into);
static bipolar host_settings_stamp(string_address path, host_settings address_to settings);
static bool host_settings_booted(host_settings address_to into);
static bool host_settings_kept(host_settings address_to into);
static fn host_settings_session(host_settings address_to settings);
static fn host_settings_keep(host_settings address_to settings);
static bool host_settings_install(host_install address_to install,
                                  host_settings address_to into);
static fn host_events_boot(host_settings address_to settings);
static fn host_bind_apply(host_settings address_to settings);
static b32 host_bind(string_address address_to arguments, positive count);
static b32 host_usage(void);
static fn host_usage_write(writer out);
static p16 host_machine_hook_line(p8 hook);
static p16 host_machine_event_line(unsigned int event);
static string_address host_machine_where(void);
static fn host_machine_refused(string_address name, p16 line);
static bool host_machine_stop(void);
static b32 host_machine_run(void);
static b32 host_radio(string_address address_to arguments, positive count);
static fn radio_restore(void);
static fn radio_recover(void);
static b32 host_locale(string_address address_to arguments, positive count);
static fn locale_restore(void);
static fn locale_recover(void);
static b32 host_wipe(void);

static b32 host_refuse(string_address text, string_address name)
{
        string_format(log_error, host_label);
        string_format(log_error, text, name);
        log_flush();
        return 1;
}

static b32 host_fail(string_address what, bipolar error)
{
        string_format(log_error, host_label "%s: %s\n", what, file_reason(error));
        log_flush();
        return 1;
}

/* Both halves must fit, or into is truncated and the answer says so. */
static bool host_join(p8 address_to into, positive room, string_address left,
                      string_address right)
{
        return string_copy_bounded(into, left, room) < room &&
               string_append_bounded(into, right, room) < room;
}

static bool host_starts(string_address text, string_address prefix)
{
        return !string_compare_max(text, prefix, string_length(prefix));
}

static fn host_pause(p64 nanoseconds)
{
        timespec span = {nanoseconds / 1000000000, nanoseconds % 1000000000};

        sleep(address_of span);
}

/* A small sysfs or state file, without its trailing newline. */
static bipolar host_read_text(string_address path, p8 address_to into,
                              positive room)
{
        bipolar got = file_slurp_once_at(AT_FDCWD, path, into, room);

        if (got < 0)
        {
                into[0] = end;
                return got;
        }

        while (got > 0 && (into[got - 1] == '\n' || into[got - 1] == ' '))
                got--;

        into[got] = end;
        return got;
}

/* Bytes over a state file, made if it is not there. A choice that has to
   survive the power going is synced; a /run file that only says what this
   session is doing does not need to be. */
static bipolar host_write_file(string_address path, p8 address_to bytes,
                               positive length, positive mode, bool sync)
{
        bipolar handle = system_open_at_mode(AT_FDCWD, path,
                                             FILE_WRITE | O_CLOEXEC, mode);
        bipolar failed;

        if (handle < 0)
                return handle;

        failed = storage_format_write(handle, bytes, length, 0);
        if (!failed && sync)
                failed = system_call_1(syscall(fsync), (positive)handle);
        system_close(handle);
        return failed;
}

static bipolar host_write_text(string_address path, string_address text)
{
        return host_write_file(path, (p8 address_to)text, string_length(text),
                               0644, false);
}

/* One line from standard input, or negative at its end. */
static bipolar host_read_line(p8 address_to into, positive room)
{
        positive used = 0;

        for (;;)
        {
                p8 byte;
                bipolar got = system_read_once(0, address_of byte, 1);

                if (got == -4)
                        continue;
                if (got <= 0)
                {
                        if (!used)
                                return -1;
                        break;
                }
                if (byte == '\n')
                        break;
                if (used + 1 < room)
                        into[used++] = byte;
        }

        while (used && (into[used - 1] == '\r' || into[used - 1] == ' '))
                used--;

        into[used] = end;
        return (bipolar)used;
}

typedef bool (*host_entry_visitor)(string_address directory,
                                   string_address name, address_any context);

static fn host_each_entry(string_address path, host_entry_visitor visit,
                          address_any context)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
                return;

        while ((entry = file_walk_next(address_of walk)))
                if (entry->d_name[0] != '.' &&
                    !visit(path, entry->d_name, context))
                        break;

        file_walk_close(address_of walk);
}

static fn host_state_ready(void)
{
        system_make_directory_at(AT_FDCWD, "/run", 0755);
        system_make_directory_at(AT_FDCWD, HOST_STATE, 0755);
}

static fn host_verdict_set(string_address kind, string_address disk)
{
        p8 line[HOST_NAME_ROOM + 16];
        p8 text[HOST_NAME_ROOM + 16];

        if (!host_join(line, sizeof(line), kind, disk) ||
            !host_join(text, sizeof(text), line, "\n"))
                return;

        if (!host_write_text(HOST_VERDICT_NEXT, text))
                system_rename_at(AT_FDCWD, HOST_VERDICT_NEXT, AT_FDCWD,
                                 HOST_VERDICT, 0);
}

// Disks ---------------------------------------------------------

static bool host_name_valid(string_address name)
{
        if (!*name || string_length(name) >= HOST_NAME_ROOM)
                return false;

        for (; *name; name++)
                if (!byte_is_alnum(*name) && *name != '-' && *name != '_')
                        return false;

        return true;
}

/* The whole disk a block device belongs to: itself, unless it is a partition. */
static bool host_parent(string_address name, p8 address_to into, positive room)
{
        p8 sysfs[HOST_PATH_ROOM];
        p8 partition[HOST_PATH_ROOM];
        p8 link[HOST_PATH_ROOM * 2];
        positive last;
        positive before;

        if (!host_name_valid(name) ||
            !host_join(sysfs, sizeof(sysfs), "/sys/class/block/", name) ||
            !host_join(partition, sizeof(partition), sysfs, "/partition"))
                return false;

        if (system_access_at(AT_FDCWD, partition, 0) < 0)
                return string_length(name) < room &&
                       (string_copy(into, name), true);

        if (file_link_text(sysfs, link, sizeof(link)) <= 0)
                return false;

        last = string_length(link);
        while (last && link[last - 1] != '/')
                last--;
        if (!last)
                return false;

        before = --last;
        while (before && link[before - 1] != '/')
                before--;

        if (last - before >= room || last == before)
                return false;

        memory_copy(into, link + before, last - before);
        into[last - before] = end;
        return true;
}

static bool host_census_visit(storage_identity address_to identity,
                              address_any opaque)
{
        host_census address_to census = (host_census address_to)opaque;
        bool system = string_equals(identity->partlabel, HOST_SYSTEM_NAME);
        bool data = string_equals(identity->partlabel, HOST_DATA_NAME);
        string_address name = identity->path + sizeof("/dev/") - 1;
        p8 parent[HOST_NAME_ROOM];
        host_install address_to install = null;

        /* Moonwater installs are GPT, so the partition UUID is the stable
           identity carried across every later mount. A label without one is
           not one of ours strongly enough to act on. */
        if ((!system && !data) || !identity->partuuid_length ||
            !host_parent(name, parent, sizeof(parent)))
                return true;

        for (positive at = 0; at < census->count; at++)
                if (string_equals(census->found[at].disk, parent))
                        install = census->found + at;

        if (!install)
        {
                if (census->count == HOST_INSTALLS)
                        return true;

                install = census->found + census->count++;
                memory_zero(install, sizeof(address_to install));
                string_copy(install->disk, parent);
        }

        string_copy(system ? install->system : install->data, name);
        string_copy(system ? install->system_partuuid : install->data_partuuid,
                    identity->partuuid);
        return true;
}

/* Every disk with both of Moonwater's partitions on it, by their names and
   stable GPT identities. */
static fn host_census_take(host_census address_to census)
{
        positive kept = 0;

        memory_zero(census, sizeof(address_to census));
        storage_each_device(host_census_visit, census);

        for (positive at = 0; at < census->count; at++)
                if (census->found[at].system[0] && census->found[at].data[0])
                        census->found[kept++] = census->found[at];

        census->count = kept;
}

static host_install address_to host_census_find(host_census address_to census,
                                                string_address disk)
{
        for (positive at = 0; at < census->count; at++)
                if (string_equals(census->found[at].disk, disk))
                        return census->found + at;

        return null;
}

typedef struct
{
        string_address prefix;
        p8 name[HOST_NAME_ROOM];
        bool seen;
} host_prefix_look;

static bool host_prefix_visit(string_address directory, string_address name,
                              address_any opaque)
{
        host_prefix_look address_to look = (host_prefix_look address_to)opaque;

        if (!host_starts(name, look->prefix) || string_length(name) >= HOST_NAME_ROOM)
                return true;

        string_copy(look->name, name);
        look->seen = true;
        return false;
}

static bool host_has_entry(string_address directory, string_address prefix,
                           p8 address_to name)
{
        host_prefix_look look;

        memory_zero(address_of look, sizeof(look));
        look.prefix = prefix;
        host_each_entry(directory, host_prefix_visit, address_of look);

        if (name && look.seen)
                string_copy(name, look.name);

        return look.seen;
}

/* A controller not live yet, or live and not yet showing a namespace. */
static bool host_nvme_visit(string_address directory, string_address name,
                            address_any opaque)
{
        p8 path[HOST_PATH_ROOM];
        p8 state[32];
        p8 prefix[HOST_NAME_ROOM + 2];

        if (!host_join(path, sizeof(path), directory, "/") ||
            !host_join(path, sizeof(path), path, name) ||
            !host_join(prefix, sizeof(prefix), name, "n"))
                return true;

        if (!host_has_entry(path, prefix, null))
        {
                address_to (bool address_to)opaque = true;
                return false;
        }

        if (!host_join(path, sizeof(path), path, "/state") ||
            host_read_text(path, state, sizeof(state)) < 0 ||
            !string_equals(state, "live"))
        {
                address_to (bool address_to)opaque = true;
                return false;
        }

        return true;
}

/* A mass-storage interface whose SCSI host has no target yet. */
static bool host_usb_visit(string_address directory, string_address name,
                           address_any opaque)
{
        p8 path[HOST_PATH_ROOM];
        p8 host[HOST_NAME_ROOM];

        if (!string_first_of(name, ':') ||
            !host_join(path, sizeof(path), directory, "/") ||
            !host_join(path, sizeof(path), path, name))
                return true;

        if (!host_has_entry(path, "host", host) ||
            !host_join(path, sizeof(path), path, "/") ||
            !host_join(path, sizeof(path), path, host) ||
            !host_has_entry(path, "target", null))
        {
                address_to (bool address_to)opaque = true;
                return false;
        }

        return true;
}

static bool host_storage_arriving(void)
{
        bool arriving = false;

        host_each_entry("/sys/class/nvme", host_nvme_visit, address_of arriving);
        if (!arriving)
                host_each_entry("/sys/bus/usb/drivers/usb-storage", host_usb_visit,
                                address_of arriving);

        return arriving;
}

// Builds --------------------------------------------------------

/* This kernel's version string, spelled the way the boot header spells it. */
static bool host_running_build(p8 address_to into, positive room)
{
        static p8 head[] = "Linux version ";
        file_machine machine;
        p8 banner[1024];
        positive release;
        positive version;
        positive prefix;
        bipolar got;
        string_address who;
        positive who_length = 0;

        into[0] = end;
        if (!file_machine_read(address_of machine))
                return false;

        got = host_read_text("/proc/version", banner, sizeof(banner));
        if (got <= 0)
                return false;

        release = string_length(machine.release);
        version = string_length(machine.version);
        prefix = sizeof(head) - 1 + release + 2;

        /*  The fixed compares below are exact byte spans, not string ones,
            and the builder scan after them has only the terminator to stop
            it. One short read from procfs would put both past what was read
            and into whatever the frame held, so the record has to be at
            least as long as the part being matched. */
        if ((positive)got < prefix ||
            memory_compare(banner, head, sizeof(head) - 1) ||
            memory_compare(banner + sizeof(head) - 1, machine.release, release) ||
            memory_compare(banner + sizeof(head) - 1 + release, " (", 2))
                return false;

        who = banner + prefix;
        while (who[who_length] && who[who_length] != ')')
                who_length++;

        if (!who[who_length] || release + who_length + version + 5 > room)
                return false;

        memory_copy(into, machine.release, release);
        memory_copy(into + release, " (", 2);
        memory_copy(into + release + 2, who, who_length);
        memory_copy(into + release + 2 + who_length, ") ", 2);
        memory_copy(into + release + 4 + who_length, machine.version, version + 1);
        return true;
}

/*
        Bytes a disk chose, before a terminal reads them.

        Two strings here are the medium's rather than this machine's: the
        version an image carries in its setup header, and the model a device
        answers an INQUIRY with under /sys. Both are written to a terminal --
        one by moonwater status beside the disk it came from, the other in
        the line asking whether to erase a disk -- and neither is filtered by
        anything between the medium and here, so somebody who plugs in a disk
        chooses what those lines do. A real banner and a real model are ASCII
        throughout, so nothing legitimate changes and an image still compares
        equal to the running build.
*/
static fn host_plain_line(p8 address_to text)
{
        for (positive at = 0; text[at]; at++)
                if (text[at] < 0x20 || text[at] > 0x7e)
                        text[at] = '?';
}

/* The version string in an x86 boot image's setup header, if it has one. */
static bool host_image_build(string_address path, p8 address_to into,
                             positive room)
{
        p8 header[0x210];
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        bool found = false;

        into[0] = end;
        if (handle < 0)
                return false;

        if (file_transfer_exact(syscall(pread64), handle, header, sizeof(header),
                                0) == (bipolar)sizeof(header) &&
            header[0] == 'M' && header[1] == 'Z' &&
            !memory_compare(header + 0x202, "HdrS", 4) &&
            storage_le16(header + 0x206) >= 0x0200 &&
            storage_le16(header + 0x20e))
        {
                bipolar got = system_call_4(syscall(pread64), (positive)handle,
                                            (positive)into, room - 1,
                                            0x200 + (positive)storage_le16(header + 0x20e));

                if (got > 0)
                {
                        into[got] = end;
                        found = into[0] && string_length(into) < (positive)got;
                        if (found)
                                host_plain_line(into);
                }
        }

        system_close(handle);
        if (!found)
                into[0] = end;

        return found;
}

static bipolar host_mount(string_address name, string_address target,
                          string_address type, positive flags)
{
        p8 device[HOST_NAME_ROOM + 8];
        bipolar made;

        if (!host_join(device, sizeof(device), "/dev/", name))
                return -ERROR_INVALID;

        host_state_ready();
        made = system_make_directory_at(AT_FDCWD, target, 0700);
        if (made < 0 && made != -EEXIST)
                return made;

        return system_mount(device, target, type, flags, 0);
}

/* Resolve the identity again immediately before a later use. Device names are
   presentation, not authority: a terminal can sit at the install/update prompt
   long enough for hotplug to reuse sda or an nvme namespace number. */
static bool host_partition_uuid_resolve(string_address uuid,
                                        p8 address_to name)
{
        p8 query[sizeof("PARTUUID=") - 1 + STORAGE_PARTUUID_ROOM];
        p8 path[HOST_PATH_ROOM];

        if (!uuid[0] ||
            !host_join(query, sizeof(query), "PARTUUID=", uuid) ||
            !storage_resolve_tag(query, path, sizeof(path)) ||
            !host_starts(path, "/dev/") ||
            !host_name_valid(path + sizeof("/dev/") - 1))
                return false;

        string_copy(name, path + sizeof("/dev/") - 1);
        return true;
}

/* Both partitions must still exist and still belong to one physical disk.
   Refresh all display names together so the rest of an operation cannot mix
   one old name with one newly resolved identity. */
static bool host_install_refresh(host_install address_to install)
{
        p8 system[HOST_NAME_ROOM];
        p8 data[HOST_NAME_ROOM];
        p8 system_disk[HOST_NAME_ROOM];
        p8 data_disk[HOST_NAME_ROOM];

        if (!host_partition_uuid_resolve(install->system_partuuid, system) ||
            !host_partition_uuid_resolve(install->data_partuuid, data) ||
            !host_parent(system, system_disk, sizeof(system_disk)) ||
            !host_parent(data, data_disk, sizeof(data_disk)) ||
            !string_equals(system_disk, data_disk))
                return false;

        string_copy(install->disk, system_disk);
        string_copy(install->system, system);
        string_copy(install->data, data);
        return true;
}

static fn host_unmount(string_address target)
{
        system_call_2(syscall(umount2), (positive)target, 0);
}

static fn host_install_read(host_install address_to install)
{
        p8 path[HOST_PATH_ROOM];

        install->readable = false;
        install->build[0] = end;

        if (!host_install_refresh(install) ||
            host_mount(install->system, HOST_LOOK, "vfat", HOST_READ_ONLY) < 0)
                return;

        if (host_join(path, sizeof(path), HOST_LOOK, HOST_IMAGE))
                install->readable = host_image_build(path, install->build,
                                                     sizeof(install->build));

        host_unmount(HOST_LOOK);
}

typedef struct
{
        string_address build;
        string_address skip;
        p8 name[HOST_NAME_ROOM];
        p8 disk[HOST_NAME_ROOM];
        bool found;
} host_medium_search;

/* A FAT partition whose image is this build. The one found stays mounted. */
static bool host_medium_visit(storage_identity address_to identity,
                              address_any opaque)
{
        host_medium_search address_to search = (host_medium_search address_to)opaque;
        string_address name = identity->path + sizeof("/dev/") - 1;
        p8 parent[HOST_NAME_ROOM];
        p8 path[HOST_PATH_ROOM];
        p8 build[HOST_BUILD_ROOM];

        if (!string_equals(identity->type, "vfat") ||
            !host_parent(name, parent, sizeof(parent)) ||
            (search->skip && string_equals(parent, search->skip)))
                return true;

        if (host_mount(name, HOST_MEDIUM, "vfat", HOST_READ_ONLY) < 0)
                return true;

        if (host_join(path, sizeof(path), HOST_MEDIUM, HOST_IMAGE) &&
            host_image_build(path, build, sizeof(build)) &&
            string_equals(build, search->build))
        {
                string_copy(search->name, name);
                string_copy(search->disk, parent);
                search->found = true;
                return false;
        }

        host_unmount(HOST_MEDIUM);
        return true;
}

/*
        A stick can still be on its way when this is asked. Boot stops looking
        the moment it finds an install, so on a machine with one the shell --
        and the question -- can be ready before usb-storage has scanned the
        stick the answer needs. A search that finds nothing keeps looking
        while the machine is young or storage is still arriving.
*/
static bool host_medium_find(host_medium_search address_to search,
                             string_address build, string_address skip)
{
        for (;;)
        {
                p64 uptime;

                memory_zero(search, sizeof(address_to search));
                search->build = build;
                search->skip = skip;
                storage_each_device(host_medium_visit, search);

                uptime = system_clock_ns(HOST_CLOCK_BOOTTIME);
                if (search->found || uptime >= HOST_SETTLE_MOST_NS ||
                    (uptime >= HOST_MEDIUM_FLOOR_NS && !host_storage_arriving()))
                        return search->found;

                host_pause(HOST_POLL_NS);
        }
}

// Placing an image ----------------------------------------------

static b32 host_copy_file(string_address from, string_address to)
{
        bipolar source = system_open_at(AT_FDCWD, from, FILE_READ | O_CLOEXEC);
        bipolar target;
        bool copied;
        bipolar synced = 0;

        if (source < 0)
                return host_fail(from, source);

        target = system_open_at_mode(AT_FDCWD, to, FILE_WRITE | O_CLOEXEC, 0644);
        if (target < 0)
        {
                system_close(source);
                return host_fail(to, target);
        }

        copied = file_copy_handles(source, target);
        if (copied)
                synced = system_call_1(syscall(fsync), (positive)target);

        system_close(source);
        system_close(target);
        if (!copied)
                return host_refuse("%s could not be copied whole\n", from);

        return synced < 0 ? host_fail(to, synced) : 0;
}

/*
        The running image onto a mounted system partition, which is the whole
        of an update. The image there before is kept as previous.efi, for the
        firmware's shell or a stick to start when a build turns out bad; the
        new one is written beside the old and renamed over it, so a machine
        that loses power part way still has an image that starts.
*/
static b32 host_place_image(string_address system, string_address running,
                            host_settings address_to carry)
{
        p8 image[HOST_PATH_ROOM];
        p8 next[HOST_PATH_ROOM];
        p8 kept[HOST_PATH_ROOM];
        p8 source[HOST_PATH_ROOM];
        p8 build[HOST_BUILD_ROOM];
        bipolar failed;

        if (!host_join(image, sizeof(image), system, "/EFI") ||
            (system_make_directory_at(AT_FDCWD, image, 0755),
             !host_join(image, sizeof(image), system, "/EFI/BOOT")) ||
            (system_make_directory_at(AT_FDCWD, image, 0755),
             !host_join(image, sizeof(image), system, HOST_KEPT_DIRECTORY)) ||
            (system_make_directory_at(AT_FDCWD, image, 0755),
             !host_join(image, sizeof(image), system, HOST_IMAGE)) ||
            !host_join(source, sizeof(source), HOST_MEDIUM, HOST_IMAGE))
                return host_refuse("%s: path too long\n", system);

        if (system_access_at(AT_FDCWD, image, 0) >= 0 &&
            host_join(next, sizeof(next), system, HOST_KEPT_IMAGE_NEXT) &&
            host_join(kept, sizeof(kept), system, HOST_KEPT_IMAGE))
        {
                if (host_copy_file(image, next))
                        return 1;

                failed = system_rename_at(AT_FDCWD, next, AT_FDCWD, kept, 0);
                if (failed < 0)
                        return host_fail(kept, failed);
        }

        if (!host_join(next, sizeof(next), system, HOST_IMAGE_NEXT) ||
            host_copy_file(source, next))
                return 1;

        //      Both slots of the new image say what the disk is to keep, before
        //      the rename makes it the one that starts.
        failed = carry ? host_settings_stamp(next, carry) : 0;
        if (failed < 0)
                return host_fail(next, failed);

        failed = system_rename_at(AT_FDCWD, next, AT_FDCWD, image, 0);
        if (failed < 0)
                return host_fail(image, failed);

        if (!host_image_build(image, build, sizeof(build)) ||
            !string_equals(build, running))
                return host_refuse("%s did not read back as this build\n", image);

        {
                bipolar directory = system_open_at(AT_FDCWD, system,
                                                   FILE_READ | O_DIRECTORY | O_CLOEXEC);

                if (directory >= 0)
                {
                        system_call_1(syscall(syncfs), (positive)directory);
                        system_close(directory);
                }
        }

        return 0;
}

// Keeping data --------------------------------------------------

static bool host_publish_visit(string_address directory, string_address name,
                               address_any unused)
{
        bowl_publish_bin(name);
        return true;
}

/*
        The data partition, mounted, over the directories it keeps.

        Binds, not a new root: the Moonwater running is the one that booted,
        whichever disk its data comes from. Bowl's launchers are published on
        /bin again, since /bin is the image's and forgot them at power off.
*/
static b32 host_attach(host_install address_to install)
{
        bipolar failed;
        positive at;

        if (!host_install_refresh(install))
                return host_refuse("%s is no longer here\n", install->disk);

        failed = host_mount(install->data, HOST_DATA, "ext4", 0);

        if (failed < 0)
                return host_fail(install->data, failed);

        for (at = 0; at < array_count(host_kept); at++)
        {
                p8 on_disk[HOST_PATH_ROOM];

                host_join(on_disk, sizeof(on_disk), HOST_DATA, host_kept[at].path);
                failed = system_make_directory_at(AT_FDCWD, on_disk, host_kept[at].mode);
                if (failed < 0 && failed != -EEXIST)
                        break;

                failed = system_make_directory_at(AT_FDCWD, host_kept[at].path,
                                                  host_kept[at].mode);
                if (failed < 0 && failed != -EEXIST)
                        break;

                failed = system_mount(on_disk, host_kept[at].path, 0, MS_BIND, 0);
                if (failed < 0)
                        break;
        }

        if (at < array_count(host_kept))
        {
                b32 said = host_fail(host_kept[at].path, failed);

                while (at-- > 0)
                        system_call_2(syscall(umount2), (positive)host_kept[at].path,
                                      MNT_DETACH);
                system_call_2(syscall(umount2), (positive)HOST_DATA, MNT_DETACH);
                return said;
        }

        bowl_mkdir(BOWL_EXPOSE_DIRECTORY);
        host_each_entry(BOWL_EXPOSE_DIRECTORY, host_publish_visit, null);
        return 0;
}

static b32 host_update(host_install address_to install)
{
        p8 running[HOST_BUILD_ROOM];
        host_medium_search search;
        bipolar mounted;
        b32 failed;

        if (!host_running_build(running, sizeof(running)))
                return host_refuse("%s cannot read its own build\n", "moonwater");

        if (!host_install_refresh(install))
                return host_refuse("%s is no longer here\n", install->disk);

        if (!host_medium_find(address_of search, running, install->disk))
                return host_refuse("this session's image is on no disk but %s, "
                                   "so there is nothing to update from\n",
                                   install->disk);

        mounted = host_mount(install->system, HOST_SYSTEM, "vfat", HOST_WRITABLE);
        if (mounted < 0)
        {
                host_unmount(HOST_MEDIUM);
                return host_fail(install->system, mounted);
        }

        string_format(log, host_label "writing this build to %s, from %s\n",
                      install->system, search.name);
        log_flush();

        /*      An update keeps the disk's settings: they are that machine's,
                and the stick is only carrying a build. An image from before
                there were settings has none, which is the defaults. */
        {
                host_settings disk;
                host_settings session;
                p8 path[HOST_PATH_ROOM];
                bool blank = true;

                host_settings_empty(address_of disk);
                if (host_join(path, sizeof(path), HOST_SYSTEM, HOST_IMAGE))
                        host_settings_image(path, address_of disk);

                disk.generation++;
                for (positive at = 0; at < sizeof(disk.medium); at++)
                        blank &= !disk.medium[at];
                if (blank)
                        system_random_fill(disk.medium, sizeof(disk.medium), 0);

                failed = host_place_image(HOST_SYSTEM, running, address_of disk);

                if (!failed)
                {
                        //      A session started from this disk is still its copy.
                        if (host_settings_kept(address_of session) &&
                            !memory_compare(session.medium, disk.medium,
                                            sizeof(disk.medium)))
                        {
                                session.generation = disk.generation;
                                host_settings_keep(address_of session);
                        }

                        string_format(log, host_label "%s keeps its own settings\n",
                                      install->disk);
                        log_flush();
                }
        }

        host_unmount(HOST_SYSTEM);
        host_unmount(HOST_MEDIUM);
        return failed;
}

/* A program to the end, its failure said; the install copies trees with cp. */
static b32 host_run(string_address address_to argv)
{
        positive status = 0;
        bipolar child = system_fork();
        bipolar reaped;

        if (child < 0)
                return host_fail(argv[0], child);

        if (child == 0)
        {
                // Through the launch decision every other exec here takes.
                (void)shell_exec_file(argv[0], argv, pointer_vector_count(argv),
                                      file_environment_all());
                system_call_1(syscall(exit), 127);
        }

        do
                reaped = system_call_4(syscall(wait4), child,
                                       (positive)address_of status, 0, 0);
        while (reaped == -4);

        if (reaped < 0)
                return host_fail(argv[0], reaped);

        if (status)
                return host_refuse("%s did not finish\n", argv[0]);

        return 0;
}

/* Use an install's data, updating it first when asked, and say so. */
static b32 host_take(host_install address_to install, bool update)
{
        system_remove_at(AT_FDCWD, HOST_QUESTION, 0);

        if (update && host_update(install))
                return 1;

        if (host_attach(install))
                return 1;

        host_verdict_set("disk ", install->disk);
        string_format(log, host_label "%s, /root and /home are kept on %s\n",
                      BOWL_ROOT_DIRECTORY, install->disk);
        log_flush();
        radio_restore();
        locale_restore();
        return 0;
}

// Boot ----------------------------------------------------------

static b32 host_boot(void)
{
        p8 running[HOST_BUILD_ROOM];
        host_census census;
        host_install address_to chosen = null;
        host_settings settings;
        bool known;

        host_state_ready();
        host_running_build(running, sizeof(running));

        /*      The settings the image booted with. A kernel started without
                the EFI stub has none, and takes an install's of this build
                once the disks are found. */
        known = host_settings_booted(address_of settings);
        if (known)
        {
                host_settings_keep(address_of settings);
                host_bind_apply(address_of settings);
        }

        if (known && settings.flags & SPARK_SETTINGS_MOUNT_OFF)
        {
                host_write_text(HOST_HINT, "");
                host_verdict_set("live", "");
                string_format(log, host_label "init mount is off: nothing on this machine's "
                                              "disks is mounted this session\n");
                log_flush();
                host_events_boot(address_of settings);
                return 0;
        }

        for (;;)
        {
                p64 uptime = system_clock_ns(HOST_CLOCK_BOOTTIME);

                host_census_take(address_of census);
                if (census.count || uptime >= HOST_SETTLE_MOST_NS ||
                    (uptime >= HOST_SETTLE_FLOOR_NS && !host_storage_arriving()))
                        break;

                host_pause(HOST_POLL_NS);
        }

        if (!census.count)
        {
                host_write_text(HOST_HINT, "");
                host_verdict_set("live", "");
                host_events_boot(known ? address_of settings : null);
                return 0;
        }

        for (positive at = 0; at < census.count; at++)
        {
                host_install_read(census.found + at);
                if (!chosen && running[0] && census.found[at].readable &&
                    string_equals(census.found[at].build, running))
                        chosen = census.found + at;
        }

        if (chosen)
        {
                if (!known && host_settings_install(chosen, address_of settings))
                {
                        known = true;
                        host_settings_keep(address_of settings);
                        host_bind_apply(address_of settings);
                }

                if (known && settings.flags & SPARK_SETTINGS_MOUNT_OFF)
                {
                        host_write_text(HOST_HINT, "");
                        host_verdict_set("live", "");
                        string_format(log, host_label "init mount is off: %s is not mounted "
                                                      "this session\n",
                                      chosen->disk);
                        log_flush();
                        host_events_boot(address_of settings);
                        return 0;
                }

                if (!host_take(chosen, false))
                {
                        host_events_boot(known ? address_of settings : null);
                        return 0;
                }

                host_verdict_set("live", "");
                host_events_boot(known ? address_of settings : null);
                return 1;
        }

        chosen = census.found;
        host_write_text(HOST_QUESTION, "");
        host_verdict_set("ask ", chosen->disk);
        string_format(log, host_label "%s has Moonwater installed from another build.\n"
                           host_label "A terminal will ask what to do with it, or "
                           "moonwater use, update or live answers from a shell.\n",
                      chosen->disk);
        log_flush();
        host_events_boot(known ? address_of settings : null);
        return 0;
}

/*
        The question, where somebody can see it.

        The first terminal to take the question file asks; it goes back if
        this one is closed without an answer, so the next terminal asks again.
*/
static fn host_question(string_address disk)
{
        host_census census;
        host_install address_to install;
        p8 running[HOST_BUILD_ROOM];
        p8 answer[16];

        host_census_take(address_of census);
        install = host_census_find(address_of census, disk);
        if (!install)
                return;

        host_install_read(install);
        if (!host_running_build(running, sizeof(running)))
                string_copy(running, "a build whose version cannot be read");

        string_format(log, "\n" host_label "%s already has Moonwater on it:\n\n    %s\n\n"
                           host_label "and this is\n\n    %s\n\n",
                      disk,
                      install->readable ? (string_address)install->build
                                        : (string_address)"an image whose build cannot be read",
                      running);
        string_format(log, "  1  use %s's %s, /root and /home with this Moonwater\n",
                      disk, BOWL_ROOT_DIRECTORY);
        string_format(log, "  2  update %s to this Moonwater, then use them\n", disk);
        string_format(log, "  3  neither: a live session, and %s is left alone\n\n",
                      disk);

        for (;;)
        {
                string_format(log, host_label "1, 2 or 3? [1] ");
                log_flush();

                if (host_read_line(answer, sizeof(answer)) < 0)
                        break;

                if (!answer[0] || string_equals(answer, "1") ||
                    string_equals(answer, "2"))
                {
                        if (!host_take(install, answer[0] == '2'))
                                return;
                        break;
                }

                if (string_equals(answer, "3"))
                {
                        host_verdict_set("live", "");
                        string_format(log, host_label "%s is left alone, and nothing "
                                                      "is kept this session\n", disk);
                        log_flush();
                        return;
                }
        }

        system_rename_at(AT_FDCWD, HOST_QUESTION_TAKEN, AT_FDCWD, HOST_QUESTION, 0);
}

/*
        What a terminal does before it starts its shell, in the pty's child,
        so what this prints and reads is the window.

        Boot's verdict can be a few seconds away when the compositor starts the
        first terminal, and a shell that started first would have read /root
        before the disk's /root was there. Terminals opened later find it
        written and pass straight through.
*/
fn host_terminal_opening(void)
{
        p8 verdict[HOST_NAME_ROOM + 16];
        bool said = false;

        while (host_read_text(HOST_VERDICT, verdict, sizeof(verdict)) < 0)
        {
                p64 uptime = system_clock_ns(HOST_CLOCK_BOOTTIME);

                if (uptime >= HOST_VERDICT_WAIT_NS)
                        return;

                if (!said && uptime >= HOST_LOOKING_NS)
                {
                        string_format(log, host_label "looking for Moonwater on "
                                                      "this machine's disks\n");
                        log_flush();
                        said = true;
                }

                host_pause(HOST_POLL_NS / 2);
        }

        if (host_starts(verdict, "ask ") &&
            system_rename_at(AT_FDCWD, HOST_QUESTION, AT_FDCWD, HOST_QUESTION_TAKEN,
                             0) >= 0)
                host_question(verdict + 4);
        else if (string_equals(verdict, "live") &&
                 system_rename_at(AT_FDCWD, HOST_HINT, AT_FDCWD, HOST_HINT_TAKEN,
                                  0) >= 0)
        {
                string_format(log, host_label "This is a live session: nothing is kept "
                                              "after power off.\n"
                                   host_label "moonwater install DISK puts Moonwater "
                                              "on a disk.\n");
                log_flush();
        }
}

// Install -------------------------------------------------------

/* A GPT GUID is stored with its first three fields little-endian.  Turn the
   exact sixteen bytes written into the PARTUUID spelling the kernel publishes. */
static fn host_partuuid(p8 address_to into, p8 address_to guid)
{
        p8 uuid[16];

        uuid[0] = guid[3];
        uuid[1] = guid[2];
        uuid[2] = guid[1];
        uuid[3] = guid[0];
        uuid[4] = guid[5];
        uuid[5] = guid[4];
        uuid[6] = guid[7];
        uuid[7] = guid[6];
        memory_copy(uuid + 8, guid + 8, 8);
        storage_uuid_bytes(into, uuid);
}

/* Find the exact partitions just written, not whatever later happens to own
   the disk's old /dev name.  PARTUUID is generated before the GPT write and
   carried in the table itself, so hot-unplug/name reuse cannot redirect the
   remainder of an install onto another disk. */
static bool host_partition_resolve(p8 address_to guid,
                                   p8 address_to into)
{
        p8 uuid[37];
        p8 query[sizeof("PARTUUID=") - 1 + 37];
        p8 path[HOST_PATH_ROOM];

        host_partuuid(uuid, guid);
        memory_copy(query, "PARTUUID=", sizeof("PARTUUID=") - 1);
        string_copy(query + sizeof("PARTUUID=") - 1, uuid);

        if (!storage_resolve_tag(query, path, sizeof(path)) ||
            !host_starts(path, "/dev/") ||
            !host_name_valid(path + sizeof("/dev/") - 1))
                return false;

        string_copy(into, path + sizeof("/dev/") - 1);
        return true;
}

static bool host_partitions_wait(host_install address_to install,
                                 storage_format_partition address_to parts)
{
        for (positive tries = 0; tries < 50; tries++)
        {
                install->system[0] = install->data[0] = end;

                if (host_partition_resolve(parts[0].unique, install->system) &&
                    host_partition_resolve(parts[1].unique, install->data))
                        return true;

                host_pause(HOST_POLL_NS);
        }

        return false;
}

/*
        Refusals first, then one question, then the writes.

        The boot image has to be on some other disk -- it is what gets
        copied, and a disk holding the only copy is the one thing that must
        not be erased. A disk with anything mounted is refused by the kernel
        itself: the exclusive open fails. Removable media is refused unless
        asked for, because the stick being installed from is removable too.
*/
static b32 host_install_disk(string_address asked, bool removable)
{
        string_address name = host_starts(asked, "/dev/") ? asked + 5 : asked;
        p8 sysfs[HOST_PATH_ROOM];
        p8 path[HOST_PATH_ROOM];
        p8 text[128];
        p8 device[HOST_NAME_ROOM + 8];
        p8 running[HOST_BUILD_ROOM];
        p8 answer[HOST_NAME_ROOM];
        p8 random[80];
        host_medium_search search;
        host_install target;
        storage_format_identity identity;
        storage_format_partition parts[2];
        p64 bytes = 0;
        b32 sector = 0;
        p64 sectors;
        p64 first;
        p64 last;
        p64 align;
        bipolar handle;
        bipolar failed;

        if (!host_name_valid(name) ||
            !host_join(sysfs, sizeof(sysfs), "/sys/class/block/", name) ||
            system_access_at(AT_FDCWD, sysfs, 0) < 0)
                return host_refuse("there is no disk called %s\n", name);

        if (host_join(path, sizeof(path), sysfs, "/partition") &&
            system_access_at(AT_FDCWD, path, 0) >= 0)
                return host_refuse("%s is a partition; name the whole disk\n", name);

        if (host_join(path, sizeof(path), sysfs, "/ro") &&
            host_read_text(path, text, sizeof(text)) > 0 && string_equals(text, "1"))
                return host_refuse("%s is read-only\n", name);

        if (!removable && host_join(path, sizeof(path), sysfs, "/removable") &&
            host_read_text(path, text, sizeof(text)) > 0 && string_equals(text, "1"))
                return host_refuse("%s is removable media; moonwater install "
                                   "needs --removable\n", name);

        if (!host_running_build(running, sizeof(running)))
                return host_refuse("%s cannot read its own build\n", "moonwater");

        /*      Read before anything is mounted to look for the image: an
                install takes this session's settings, which is how what was
                set on a live stick is still set on the disk it installed. */
        host_settings carry;

        host_settings_session(address_of carry);
        carry.generation++;
        system_random_fill(carry.medium, sizeof(carry.medium), 0);

        if (!host_medium_find(address_of search, running, name))
        {
                if (host_medium_find(address_of search, running, null))
                {
                        host_unmount(HOST_MEDIUM);
                        return host_refuse("the only copy of this session's image is "
                                           "on %s itself\n", name);
                }

                return host_refuse("this session's image is on no disk here, so "
                                   "there is nothing to install on %s\n", name);
        }

        host_join(device, sizeof(device), "/dev/", name);

        /*
                Pin the destructive target before describing it or asking for
                confirmation, and keep that exact open description through the
                final write.  Closing a read-only probe here and reopening
                /dev/<name> after the prompt leaves a hot-unplug/name-reuse
                window in which the operator confirms one disk and a different
                disk receives the partition table.
        */
        handle = system_open_at(AT_FDCWD, device,
                                FILE_READ_WRITE | FILE_EXCLUSIVE | O_CLOEXEC);
        if (handle < 0)
        {
                host_unmount(HOST_MEDIUM);
                return handle == -ERROR_BUSY
                           ? host_refuse("%s is in use: something on it is mounted\n",
                                         name)
                           : host_fail(device, handle);
        }

        failed = system_control(handle, HOST_BLKGETSIZE64, address_of bytes);
        if (failed >= 0)
                failed = system_control(handle, HOST_BLKSSZGET,
                                        address_of sector);

        if (failed < 0 || bytes < HOST_SMALLEST || sector < 512 ||
            !storage_gpt_span(bytes / (p64)sector, (p32)sector, address_of first,
                              address_of last))
        {
                system_close(handle);
                host_unmount(HOST_MEDIUM);
                return failed < 0 ? host_fail(device, failed)
                                  : host_refuse("%s is smaller than the 2 GiB an "
                                                "install needs\n", name);
        }

        if (!host_join(path, sizeof(path), sysfs, "/device/model") ||
            host_read_text(path, text, sizeof(text)) <= 0)
                string_copy(text, "a disk");
        host_plain_line(text);

        string_format(log, host_label "Installing erases everything on %s: %s, %p GiB.\n"
                           host_label "Type %s to go on: ",
                      name, text, bytes >> 30, name);
        log_flush();

        if (host_read_line(answer, sizeof(answer)) < 0 ||
            !string_equals(answer, name))
        {
                system_close(handle);
                host_unmount(HOST_MEDIUM);
                string_format(log, host_label "nothing written\n");
                log_flush();
                return 1;
        }

        failed = system_random_fill(random, sizeof(random), 0);

        memory_zero(parts, sizeof(parts));
        memory_copy(identity.uuid, random, 16);
        memory_copy(identity.hash_seed, random + 16, 16);
        tools_uuid_version(identity.uuid, 6, 4);
        tools_uuid_version(identity.hash_seed, 6, 4);
        tools_uuid_version(random + 32, 7, 4);
        memory_copy(parts[0].unique, random + 48, 16);
        memory_copy(parts[1].unique, random + 64, 16);
        tools_uuid_version(parts[0].unique, 7, 4);
        tools_uuid_version(parts[1].unique, 7, 4);
        identity.time = (p32)(system_clock_ns(HOST_CLOCK_REALTIME) / 1000000000);
        identity.label = "moonwater";

        //      A 512 MiB system partition at 1 MiB, then the rest, to 16 TiB.
        sectors = bytes / (p64)sector;
        align = HOST_ALIGN_BYTES / (p64)sector;
        memory_copy(parts[0].type, storage_gpt_system_type, 16);
        memory_copy(parts[1].type, storage_gpt_linux_type, 16);
        parts[0].name = HOST_SYSTEM_NAME;
        parts[1].name = HOST_DATA_NAME;
        parts[0].first = align;
        parts[0].last = align + HOST_SYSTEM_BYTES / (p64)sector - 1;
        parts[1].first = parts[0].last + 1;
        parts[1].last = (last + 1) / align * align - 1;
        if (parts[1].last - parts[1].first + 1 >
            STORAGE_EXT4_MOST * STORAGE_EXT4_BLOCK / (p64)sector)
                parts[1].last = parts[1].first +
                                STORAGE_EXT4_MOST * STORAGE_EXT4_BLOCK / (p64)sector - 1;

        if (!failed)
        {
                string_format(log, host_label "partitioning and formatting %s\n", name);
                log_flush();

                failed = storage_format_zero(handle, 0, HOST_ALIGN_BYTES);
        }
        if (!failed)
                failed = storage_format_zero(handle, bytes - HOST_ALIGN_BYTES,
                                             HOST_ALIGN_BYTES);
        if (!failed)
                failed = storage_format_gpt(handle, sectors, (p32)sector, random + 32,
                                            parts, 2);
        if (!failed)
                failed = storage_format_fat32(
                    handle, parts[0].first * (p64)sector,
                    (parts[0].last - parts[0].first + 1) * (p64)sector, (p32)sector,
                    parts[0].first, address_of identity);
        if (!failed)
                failed = storage_format_zero(handle, parts[1].first * (p64)sector,
                                             HOST_ALIGN_BYTES);
        if (!failed)
                failed = storage_format_ext4(
                    handle, parts[1].first * (p64)sector,
                    (parts[1].last - parts[1].first + 1) * (p64)sector,
                    address_of identity);
        if (!failed)
                failed = system_call_1(syscall(fsync), (positive)handle);

        for (positive tries = 0; !failed && tries < 20; tries++)
        {
                failed = system_control(handle, HOST_BLKRRPART, 0);
                if (failed != -ERROR_BUSY)
                        break;

                host_pause(HOST_POLL_NS);
        }

        system_close(handle);

        memory_zero(address_of target, sizeof(target));
        string_copy(target.disk, name);

        if (failed || !host_partitions_wait(address_of target, parts))
        {
                host_unmount(HOST_MEDIUM);
                return failed ? host_fail(device, failed)
                              : host_refuse("the new partitions on %s did not appear\n",
                                            name);
        }

        failed = host_mount(target.system, HOST_SYSTEM, "vfat", HOST_WRITABLE);
        if (failed < 0)
        {
                host_unmount(HOST_MEDIUM);
                return host_fail(target.system, failed);
        }

        string_format(log, host_label "writing this build to %s, from %s, with this "
                                      "session's settings\n",
                      target.system, search.name);
        log_flush();

        failed = host_place_image(HOST_SYSTEM, running, address_of carry);
        host_unmount(HOST_SYSTEM);
        host_unmount(HOST_MEDIUM);
        if (failed)
                return 1;

        failed = host_mount(target.data, HOST_DATA, "ext4", 0);
        if (failed < 0)
                return host_fail(target.data, failed);

        /*
                What this session already has goes along, bowls and all. Each
                directory is copied under its own name into the empty data
                partition, which host_attach then binds: this cp copies nothing
                for `cp -a dir/. existing`, and exits 1 without a word.
        */
        for (positive at = 0; at < array_count(host_kept); at++)
        {
                string_address argv[] = {"/bin/cp", "-a", host_kept[at].path,
                                         HOST_DATA "/", null};

                if (system_access_at(AT_FDCWD, host_kept[at].path, 0) < 0)
                        continue;

                string_format(log, host_label "copying %s\n", host_kept[at].path);
                log_flush();

                if (host_run(argv))
                {
                        host_unmount(HOST_DATA);
                        return 1;
                }
        }

        host_unmount(HOST_DATA);
        if (host_take(address_of target, false))
                return 1;

        string_format(log, host_label "Moonwater is installed on %s. Power off and take "
                                      "the stick out, and the machine starts from %s.\n",
                      name, name);
        log_flush();
        return 0;
}

/*
        Before the machine stops, every filesystem on a disk remounted
        read-only.

        sync puts the data on the disk, but an ext4 left mounted keeps a
        journal the next mount has to replay, and until one does, e2fsck calls
        the filesystem unclean. Remounting read-only writes the journal out
        and marks it clean, which an installed machine wants every time it
        powers off. Best effort, latest mount first: a filesystem something
        still holds open for writing refuses, and keeps what sync gave it.
*/
fn host_quiesce(void)
{
        storage_mount_table table;
        string_address done[32];
        positive done_count = 0;

        if (!storage_mount_table_load(address_of table, null))
                return;

        //      A device bound in several places is one superblock: once is all.
        for (positive at = table.count; at-- > 0;)
        {
                storage_mount address_to mount = table.entry + at;
                bool seen = false;

                if (!host_starts(mount->source, "/dev/"))
                        continue;

                for (positive look = 0; look < done_count; look++)
                        if (string_equals(done[look], mount->device))
                                seen = true;
                if (seen)
                        continue;

                if (done_count < array_count(done))
                        done[done_count++] = mount->device;

                system_mount(0, mount->target, 0, MS_REMOUNT | MS_RDONLY, 0);
        }

        storage_mount_table_release(address_of table);
}

// Bindings ------------------------------------------------------

/*
        One request to a bound event. SET when `command` is not null, then
        the kernel copies the row back. A command too long is handed over
        whole, cut at the size without a terminator, so the kernel's own
        refusal answers. The listing and boot apply keep the file open;
        opening once per event was twenty-two trips through /dev/spark for
        `moonwater bind` with no arguments.
*/
static bipolar host_spark_once(unsigned int command, void *request, unsigned int flags)
{
        bipolar device = system_open_at(AT_FDCWD, SPARK_DEVICE, flags | O_CLOEXEC);
        bipolar failed;

        if (device < 0)
                return device;

        failed = system_control(device, command, request);
        system_close(device);
        return failed;
}

static bipolar host_bind_ioctl(bipolar device, unsigned int op, unsigned int event,
                               string_address command,
                               struct bind_control address_to control)
{
        bipolar failed;
        positive length;

        memory_zero(control, sizeof(address_to control));
        control->op = op;
        control->event = event;

        if (command)
        {
                length = string_length(command);
                memory_copy(control->command, command,
                            length < SPARK_BIND_COMMAND_MAX ? length + 1
                                                            : SPARK_BIND_COMMAND_MAX);
        }

        failed = system_control(device, SPARK_IOCTL_BIND, control);
        control->command[SPARK_BIND_COMMAND_MAX - 1] = end;
        control->name[SPARK_BIND_NAME_MAX - 1] = end;
        return failed < 0 ? failed : 0;
}

static bipolar host_bind_request(unsigned int op, unsigned int event,
                                 string_address command,
                                 struct bind_control address_to control)
{
        bipolar device = system_open_at(AT_FDCWD, SPARK_DEVICE, FILE_READ | O_CLOEXEC);
        bipolar failed;

        if (device < 0)
                return device;

        failed = host_bind_ioctl(device, op, event, command, control);
        system_close(device);
        return failed;
}

// Settings ------------------------------------------------------

/*
        What this machine does at boot and when it stops, kept in the boot
        image itself.

        The block is spark.c's: two slots in the image's .mwset section. A
        change is made to this session's copy, /run/moonwater/settings, and
        then written over the older slot of the image this session started
        from. That image is found by its build and by the medium id and
        generation the session's copy carries, so of a stick and a disk with
        the same build only the one that booted is written.

        The write goes to the partition underneath the file, at the blocks the
        filesystem names, and only once those blocks have read back as the
        file's own bytes. The file keeps its size, and the FAT, the directory
        entry and its times are never touched, so power lost part way tears
        one slot and leaves the other to boot from. No image is copied or
        renamed to change a setting.

        What cannot be written -- a read-only stick, an image found on no
        disk, one built without the section -- stays this session's, says
        why, and still goes along with moonwater install.
*/
#define HOST_SETTINGS HOST_STATE "/settings"
#define HOST_SETTINGS_NEXT HOST_STATE "/settings.next"
#define HOST_SETTINGS_SECTION ".mwset\0\0"
#define HOST_SETTINGS_PAGE 4096
#define HOST_SETTINGS_BLOCKS (SPARK_SETTINGS_SLOT / 512)
#define HOST_FIBMAP 1
#define HOST_FIGETBSZ 2
#define HOST_BLKFLSBUF 0x1261
#define HOST_FADVISE_DONTNEED 4

#define HOST_SETTINGS_CHANGED 0
#define HOST_SETTINGS_SHOWN 1
#define HOST_SETTINGS_USAGE 2
#define HOST_SETTINGS_REFUSED 3

typedef struct
{
        struct spark_settings_entry entry;
        p8 address_to text;
        positive at;
} host_setting;

static const struct
{
        string_address verb;
        p8 list;
        string_address empty;
        p8 hook;
} host_lists[] = {
    {"init", SPARK_SETTINGS_INIT, "nothing runs at boot", MOONWATER_HOOK_INIT},
    {"exit", SPARK_SETTINGS_EXIT, "nothing runs when the machine stops",
     MOONWATER_HOOK_END},
};

/*
        Which command each switch is spelled under. Moving one is a line here.

        The block also holds the startup list and Canvas at boot, which nothing
        reads yet: no command takes them until Canvas does, so none is a
        setting that is accepted and then does nothing.
*/
static const struct
{
        string_address verb;
        string_address word;
        p32 flag;
} host_switches[] = {
    {"init", "mount", SPARK_SETTINGS_MOUNT_OFF},
};

static fn host_settings_empty(host_settings address_to settings)
{
        memory_zero(settings, sizeof(address_to settings));
        settings->magic = SPARK_SETTINGS_MAGIC;
        settings->version = SPARK_SETTINGS_VERSION;
        settings->header = SPARK_SETTINGS_HEADER;
        settings->slot = SPARK_SETTINGS_SLOT;
}

static fn host_settings_seal(host_settings address_to settings)
{
        settings->sum = 0;
        settings->sum = spark_settings_sum(settings);
}

/* The entry at at, stepping at past it; false once there are no more. */
static bool host_settings_next(host_settings address_to settings,
                               positive address_to at,
                               host_setting address_to into)
{
        if (address_to at + SPARK_SETTINGS_ENTRY > settings->length)
                return false;

        into->at = address_to at;
        memory_copy_apart(address_of into->entry, settings->payload + into->at,
                          SPARK_SETTINGS_ENTRY);
        into->text = settings->payload + into->at + SPARK_SETTINGS_ENTRY;
        address_to at += SPARK_SETTINGS_ENTRY +
                         spark_settings_padded(into->entry.length);
        return true;
}

/* An entry's words, the way the command line spells them. */
static fn host_settings_text(p8 address_to into, host_setting address_to setting)
{
        if (setting->entry.kind == SPARK_SETTINGS_SHELL)
                string_copy(into, "shell");
        else if (setting->entry.kind == SPARK_SETTINGS_KERNEL_SHELL)
                string_copy(into, "kernel_shell");
        else
        {
                memory_copy_apart(into, setting->text, setting->entry.length);
                into[setting->entry.length] = end;
        }
}

static positive host_settings_count(host_settings address_to settings, p8 list)
{
        host_setting setting;
        positive at = 0;
        positive count = 0;

        while (host_settings_next(settings, address_of at, address_of setting))
                count += setting.entry.list == list;

        return count;
}

/*
        An entry at the end of its list, with the next id that list hands out:
        ids are never given twice, so a remove by number cannot reach an entry
        added after the number was read. Null once it is in, else why not.
*/
static string_address host_settings_add(host_settings address_to settings,
                                        p8 list, p8 kind, string_address text,
                                        positive length, p16 address_to id)
{
        positive room = spark_settings_padded(length);
        struct spark_settings_entry entry;

        if (length > (list == SPARK_SETTINGS_BIND ? SPARK_SETTINGS_BIND_TEXT_MOST
                                                  : SPARK_SETTINGS_TEXT_MOST))
                return list == SPARK_SETTINGS_BIND ? "a bound command is 255 bytes at most"
                                                   : "an entry is 4096 bytes at most";

        if (host_settings_count(settings, list) >=
            (list == SPARK_SETTINGS_BIND ? SPARK_SETTINGS_BIND_MOST : SPARK_SETTINGS_LIST_MOST))
                return list == SPARK_SETTINGS_BIND ? "the bind table holds 48 events at most"
                                                   : "a list holds 16 entries at most";

        if (SPARK_SETTINGS_PAYLOAD - settings->length < SPARK_SETTINGS_ENTRY + room)
                return "the settings block is full";

        memory_zero(address_of entry, sizeof(entry));
        entry.list = list;
        entry.kind = kind;
        entry.length = (p16)length;

        //      A bound command's id is its event, which the caller names.
        if (list == SPARK_SETTINGS_BIND)
                entry.id = id ? address_to id : 0;
        else
        {
                p16 address_to next = settings->next + list - 1;

                entry.id = address_to next ? address_to next : 1;
                if (entry.id == 0xffff)
                        return "this list has handed out every id it has";

                address_to next = entry.id + 1;
        }

        memory_copy_apart(settings->payload + settings->length, address_of entry,
                          SPARK_SETTINGS_ENTRY);
        memory_copy_apart(settings->payload + settings->length + SPARK_SETTINGS_ENTRY,
                          text, length);
        memory_zero(settings->payload + settings->length + SPARK_SETTINGS_ENTRY +
                        length,
                    room - length);
        settings->length += SPARK_SETTINGS_ENTRY + room;

        if (id)
                address_to id = entry.id;

        return null;
}

static fn host_settings_drop(host_settings address_to settings,
                             host_setting address_to setting)
{
        positive size = SPARK_SETTINGS_ENTRY +
                        spark_settings_padded(setting->entry.length);

        memory_copy(settings->payload + setting->at,
                    settings->payload + setting->at + size,
                    settings->length - setting->at - size);
        settings->length -= size;
        memory_zero(settings->payload + settings->length, size);
}

/* An entry named by its id, all digits, or by its exact words. */
static bool host_settings_find(host_settings address_to settings, p8 list,
                               string_address wanted, host_setting address_to into)
{
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        positive id = 0;
        positive at = 0;
        bool by_id = *wanted != end;

        for (string_address digit = wanted; *digit && by_id; digit++)
        {
                by_id = byte_is_digit(*digit) && id <= 0xffff;
                id = id * 10 + (positive)(*digit - '0');
        }

        while (host_settings_next(settings, address_of at, into))
        {
                if (into->entry.list != list)
                        continue;

                if (by_id ? into->entry.id == id
                          : (host_settings_text(text, into), string_equals(text, wanted)))
                        return true;
        }

        return false;
}

/* Words given apart, joined by one space the way a shell would read them. */
static bool host_settings_words(p8 address_to into, positive room,
                                string_address address_to words, positive count,
                                positive address_to length)
{
        into[0] = end;

        for (positive at = 0; at < count; at++)
                if ((at && string_append_bounded(into, " ", room) >= room) ||
                    string_append_bounded(into, words[at], room) >= room)
                        return false;

        address_to length = string_length(into);
        return true;
}

/* Which slot a write goes over: a damaged one, else the older. */
static positive host_settings_older(host_settings address_to slots)
{
        if (spark_settings_check(slots) < 0)
                return 0;
        if (spark_settings_check(slots + 1) < 0)
                return 1;

        return slots[1].generation < slots[0].generation ? 1 : 0;
}

/* One past the newest generation either slot believably carries. */
static p64 host_settings_generation(host_settings address_to slots)
{
        p64 most = 0;

        for (positive at = 0; at < 2; at++)
                if (spark_settings_check(slots + at) >= 0 && slots[at].generation > most)
                        most = slots[at].generation;

        return most + 1;
}

/*
        Where an image keeps its settings: the file offset of its first slot,
        or 0 for an image without them. The section table says where, and both
        slots must carry the magic there before anything believes it.
*/
static p64 host_settings_section(bipolar handle)
{
        p8 head[HOST_SETTINGS_PAGE];
        p64 magic[2];
        positive pe;
        positive table;
        positive count;

        if (file_transfer_exact(syscall(pread64), handle, head, sizeof(head), 0) !=
                (bipolar)sizeof(head) ||
            head[0] != 'M' || head[1] != 'Z')
                return 0;

        pe = storage_le32(head + 0x3c);
        if (pe > sizeof(head) - 24 || memory_compare(head + pe, "PE\0\0", 4))
                return 0;

        count = storage_le16(head + pe + 6);
        table = pe + 24 + storage_le16(head + pe + 20);

        for (positive at = 0; at < count && table + (at + 1) * 40 <= sizeof(head); at++)
        {
                p8 address_to section = head + table + at * 40;
                p64 raw = storage_le32(section + 20);

                if (memory_compare(section, HOST_SETTINGS_SECTION, 8))
                        continue;

                if (!raw || raw % HOST_SETTINGS_PAGE ||
                    storage_le32(section + 16) < 2 * SPARK_SETTINGS_SLOT ||
                    file_transfer_exact(syscall(pread64), handle, (p8 address_to)magic, 8,
                                        raw) != 8 ||
                    file_transfer_exact(syscall(pread64), handle,
                                        (p8 address_to)(magic + 1), 8,
                                        raw + SPARK_SETTINGS_SLOT) != 8 ||
                    magic[0] != SPARK_SETTINGS_MAGIC || magic[1] != SPARK_SETTINGS_MAGIC)
                        return 0;

                return raw;
        }

        return 0;
}

static bool host_settings_slots(bipolar handle, host_settings address_to slots,
                                p64 address_to offset)
{
        address_to offset = host_settings_section(handle);

        return address_to offset &&
               file_transfer_exact(syscall(pread64), handle, (p8 address_to)slots,
                                   2 * SPARK_SETTINGS_SLOT, address_to offset) ==
                   (bipolar)(2 * SPARK_SETTINGS_SLOT);
}

/*
        The settings an image boots with: 1 from a slot that checks, 0 for the
        defaults because both are damaged, -1 for an image without settings.
*/
static b32 host_settings_image(string_address path, host_settings address_to into)
{
        host_settings slots[2];
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        p64 offset;
        bool read;
        b32 newest;

        host_settings_empty(into);
        if (handle < 0)
                return -1;

        read = host_settings_slots(handle, slots, address_of offset);
        system_close(handle);
        if (!read)
                return -1;

        newest = spark_settings_newest(slots);
        if (newest < 0)
                return 0;

        memory_copy_apart(into, slots + newest, SPARK_SETTINGS_SLOT);
        return 1;
}

static bool host_settings_kept(host_settings address_to into)
{
        bipolar handle = system_open_at(AT_FDCWD, HOST_SETTINGS, FILE_READ | O_CLOEXEC);
        bipolar got;

        if (handle < 0)
                return false;

        got = file_transfer_exact(syscall(pread64), handle, (p8 address_to)into,
                                  SPARK_SETTINGS_SLOT, 0);
        system_close(handle);
        return got == (bipolar)SPARK_SETTINGS_SLOT && spark_settings_check(into) >= 0;
}

/*
        The kernel's copy: what the image booted with, or what was set since.
        A kernel with no /dev/spark, or started without the stub, has none.
*/
static bool host_settings_booted(host_settings address_to into)
{
        struct spark_settings_request request = {(unsigned long)into, 0};

        return host_spark_once(SPARK_IOCTL_SETTINGS_GET, address_of request,
                               FILE_READ_WRITE) >= 0 &&
               spark_settings_check(into) >= 0;
}

/* This session's copy, root's alone because a command can carry a secret, and the kernel's. */
static fn host_settings_keep(host_settings address_to settings)
{
        struct spark_settings_request request = {(unsigned long)settings, 0};
        bipolar handle;

        host_settings_seal(settings);
        host_state_ready();
        (void)host_spark_once(SPARK_IOCTL_SETTINGS_SET, address_of request,
                              FILE_READ_WRITE);

        handle = system_open_at_mode(AT_FDCWD, HOST_SETTINGS_NEXT,
                                     FILE_WRITE | O_CLOEXEC, 0600);
        if (handle < 0)
                return;

        if (!storage_format_write(handle, (p8 address_to)settings,
                                  SPARK_SETTINGS_SLOT, 0))
                system_rename_at(AT_FDCWD, HOST_SETTINGS_NEXT, AT_FDCWD,
                                 HOST_SETTINGS, 0);

        system_close(handle);
}

typedef struct
{
        string_address build;
        host_settings address_to session;
        p8 name[HOST_NAME_ROOM];
        p8 other[HOST_NAME_ROOM];
        positive count;
        host_settings found;
} host_settings_search;

/* A FAT partition whose image is this build and, when asked, this session's copy. */
static bool host_settings_visit(storage_identity address_to identity,
                                address_any opaque)
{
        host_settings_search address_to search = (host_settings_search address_to)opaque;
        string_address name = identity->path + sizeof("/dev/") - 1;
        p8 path[HOST_PATH_ROOM];
        p8 build[HOST_BUILD_ROOM];
        host_settings newest;

        if (!string_equals(identity->type, "vfat") || !host_name_valid(name) ||
            host_mount(name, HOST_MEDIUM, "vfat", HOST_READ_ONLY) < 0)
                return true;

        if (host_join(path, sizeof(path), HOST_MEDIUM, HOST_IMAGE) &&
            host_image_build(path, build, sizeof(build)) &&
            string_equals(build, search->build) &&
            host_settings_image(path, address_of newest) >= 0 &&
            (!search->session ||
             (newest.generation == search->session->generation &&
              !memory_compare(newest.medium, search->session->medium,
                              sizeof(newest.medium)))))
        {
                if (!search->count)
                {
                        string_copy(search->name, name);
                        memory_copy_apart(address_of search->found, address_of newest,
                                          SPARK_SETTINGS_SLOT);
                }
                else if (search->count == 1)
                        string_copy(search->other, name);

                search->count++;
        }

        host_unmount(HOST_MEDIUM);
        return true;
}

/*
        What a disk nobody has authenticated is allowed to say.

        The only thing tying a settings image on some disk to this build is
        the version string in that image's own setup header, and that string
        is bytes in a file: anybody who can read /proc/version can write it
        into an image of their own. Believing it about the two switches is a
        nuisance at worst. Believing it about the lists hands whoever pushed
        the disk in what this machine runs -- every entry in the payload is a
        command, at boot, at stop, or on a bound event, and these settings do
        not stay in memory: an install stamps them into the image it writes,
        and moonwater bind saves them as this session's. So the switches are
        taken and the payload is not, and the startup flag goes with it
        because it says a list was written.
*/
static fn host_settings_untrusted(host_settings address_to settings)
{
        memory_zero(settings->payload, settings->length);
        settings->length = 0;
        settings->flags &= ~SPARK_SETTINGS_STARTUP_SET;
        memory_zero(settings->next, sizeof(settings->next));
        host_settings_seal(settings);
}

/* This session's settings: its own copy, else the one image of this build a disk here has, else the defaults. */
static fn host_settings_session(host_settings address_to settings)
{
        host_settings_search search;
        p8 running[HOST_BUILD_ROOM];

        if (host_settings_kept(settings) || host_settings_booted(settings))
                return;

        host_settings_empty(settings);

        /*  Tighter than safe hears nothing from a disk at all: the switches
            are only a nuisance, but a machine that wants no word from media
            somebody pushed in gets none. */
        if (MOONWATER_STRICT >= STRICT_TIGHT)
                return;

        memory_zero(address_of search, sizeof(search));

        if (!host_running_build(running, sizeof(running)))
                return;

        search.build = running;
        storage_each_device(host_settings_visit, address_of search);

        if (search.count != 1)
                return;

        memory_copy_apart(settings, address_of search.found, SPARK_SETTINGS_SLOT);
        host_settings_untrusted(settings);
}

typedef struct
{
        host_settings slots[2];
        p64 offset;
        b32 blocks[HOST_SETTINGS_BLOCKS];
        positive size;
        positive target;
} host_settings_place;

/* The partition blocks under the slot a write goes over, as its filesystem names them. */
static string_address host_settings_map(string_address name,
                                        host_settings_place address_to place)
{
        p8 path[HOST_PATH_ROOM];
        string_address failed = null;
        bipolar handle = -ERROR_INVALID;
        b32 size = 0;

        if (host_mount(name, HOST_MEDIUM, "vfat", HOST_READ_ONLY) < 0)
                return "could not be mounted";

        if (host_join(path, sizeof(path), HOST_MEDIUM, HOST_IMAGE))
                handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);

        if (handle < 0)
                failed = "has no image";
        else if (!host_settings_slots(handle, place->slots, address_of place->offset))
                failed = "has an image built without settings";
        else if (system_control(handle, HOST_FIGETBSZ, address_of size) < 0 ||
                 size < 512 || size > HOST_SETTINGS_PAGE || HOST_SETTINGS_PAGE % size)
                failed = "keeps its image in blocks this cannot map";

        place->size = failed ? 512 : (positive)size;
        place->target = host_settings_older(place->slots);

        for (positive at = 0; !failed && at < SPARK_SETTINGS_SLOT / place->size; at++)
        {
                b32 block = (b32)((place->offset + place->target * SPARK_SETTINGS_SLOT) /
                                      place->size +
                                  at);

                if (system_control(handle, HOST_FIBMAP, address_of block) < 0 || block <= 0)
                        failed = "keeps its image in blocks this cannot map";

                place->blocks[at] = block;
        }

        if (handle >= 0)
                system_close(handle);

        host_unmount(HOST_MEDIUM);
        return failed;
}

/*
        This session's settings over the older slot of the image on name.

        Nothing else may have the partition mounted, and the blocks must hold
        the file's bytes before one is written. Afterwards the partition's
        cache is flushed and the file read again from a fresh mount with its
        pages dropped, so what is checked is the disk and not memory. Null
        once written and read back, else how it went wrong, said after the
        partition's name.
*/
static string_address host_settings_write(string_address name,
                                          host_settings address_to settings)
{
        host_settings_place place;
        host_settings slot;
        storage_mount_table table;
        p8 device[HOST_NAME_ROOM + 8];
        p8 path[HOST_PATH_ROOM];
        p8 text[8];
        p8 block[HOST_SETTINGS_PAGE];
        string_address failed = null;
        bipolar handle;
        p64 offset = 0;
        b32 newest;
        bool blank = true;

        if (!host_join(device, sizeof(device), "/dev/", name) ||
            !host_join(path, sizeof(path), "/sys/class/block/", name) ||
            !host_join(path, sizeof(path), path, "/ro"))
                return "has a name too long to write";

        if (host_read_text(path, text, sizeof(text)) > 0 && string_equals(text, "1"))
                return "is read-only";

        if (storage_mount_table_load(address_of table, null))
        {
                bool mounted = false;

                for (positive at = 0; at < table.count; at++)
                        mounted |= string_equals(table.entry[at].source, device);

                storage_mount_table_release(address_of table);
                if (mounted)
                        return "is mounted; unmount it and try again";
        }

        failed = host_settings_map(name, address_of place);
        if (failed)
                return failed;

        memory_copy_apart(address_of slot, settings, SPARK_SETTINGS_SLOT);
        slot.generation = host_settings_generation(place.slots);

        for (positive at = 0; at < sizeof(slot.medium); at++)
                blank &= !slot.medium[at];

        if (blank)
        {
                newest = spark_settings_newest(place.slots);
                if (newest >= 0)
                        memory_copy_apart(slot.medium, place.slots[newest].medium,
                                          sizeof(slot.medium));

                blank = true;
                for (positive at = 0; at < sizeof(slot.medium); at++)
                        blank &= !slot.medium[at];

                if (blank)
                        system_random_fill(slot.medium, sizeof(slot.medium), 0);
        }

        host_settings_seal(address_of slot);

        handle = system_open_at(AT_FDCWD, device,
                                FILE_READ_WRITE | FILE_EXCLUSIVE | O_CLOEXEC);
        if (handle < 0)
                return handle == -ERROR_BUSY ? "is in use; unmount it and try again"
                       : handle == -ERROR_READ_ONLY || handle == -ERROR_ACCESS
                           ? "is read-only"
                           : "cannot be opened for writing";

        for (positive at = 0; !failed && at < SPARK_SETTINGS_SLOT / place.size; at++)
                if (file_transfer_exact(syscall(pread64), handle, block, place.size,
                                        (p64)place.blocks[at] * place.size) !=
                        (bipolar)place.size ||
                    memory_compare(block,
                                   (p8 address_to)(place.slots + place.target) +
                                       at * place.size,
                                   place.size))
                        failed = "does not keep its image where its filesystem says, "
                                 "so nothing was written";

        for (positive at = 0; !failed && at < SPARK_SETTINGS_SLOT / place.size; at++)
                if (storage_write(handle, (p8 address_to)address_of slot + at * place.size,
                                  place.size, (p64)place.blocks[at] * place.size) !=
                    (bipolar)place.size)
                        failed = "could not be written; its other slot still has the "
                                 "settings from before";

        if (!failed && system_call_1(syscall(fsync), (positive)handle) < 0)
                failed = "could not be synced; its other slot still has the settings "
                         "from before";

        system_control(handle, HOST_BLKFLSBUF, 0);
        system_close(handle);
        if (failed)
                return failed;

        if (host_mount(name, HOST_MEDIUM, "vfat", HOST_READ_ONLY) < 0)
                return "was written, and could not be mounted again to check it";

        handle = host_join(path, sizeof(path), HOST_MEDIUM, HOST_IMAGE)
                     ? system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC)
                     : -ERROR_INVALID;

        if (handle >= 0)
        {
                system_call_4(syscall(fadvise64), (positive)handle, 0, 0,
                              HOST_FADVISE_DONTNEED);
                if (!host_settings_slots(handle, place.slots, address_of offset))
                        offset = 0;
                system_close(handle);
        }

        host_unmount(HOST_MEDIUM);

        if (offset != place.offset ||
            memory_compare(place.slots + place.target, address_of slot,
                           SPARK_SETTINGS_SLOT) ||
            spark_settings_newest(place.slots) != (b32)place.target)
                return "was written and did not read back as written";

        settings->generation = slot.generation;
        memory_copy_apart(settings->medium, slot.medium, sizeof(slot.medium));
        return null;
}

/* Writes this session's settings to its image where it can, ending the line with where they went. */
static fn host_settings_save(host_settings address_to settings)
{
        host_settings_search search;
        p8 running[HOST_BUILD_ROOM];
        string_address failed = null;

        memory_zero(address_of search, sizeof(search));

        if (host_running_build(running, sizeof(running)))
        {
                search.build = running;
                search.session = settings;
                storage_each_device(host_settings_visit, address_of search);
        }

        if (search.count == 1)
                failed = host_settings_write(search.name, settings);

        if (search.count == 1 && !failed)
                string_format(log, "saved in the image on %s\n", search.name);
        else if (search.count == 1)
                string_format(log, "this session only: %s %s\n", search.name, failed);
        else if (search.count)
                string_format(log, "this session only: %s and %s both have this "
                                   "session's image\n",
                              search.name, search.other);
        else
                string_format(log, "this session only: no disk here has the image "
                                   "this session started from\n");

        log_flush();
        host_settings_keep(settings);
}

/*
        One list said twice.

        `moonwater init` writes it as log lines of its own, and `moonwater
        status` sets the same three answers -- the hook that took the list
        away, the entries, or the sentence for an empty one -- under a
        heading on the session page. Only the frame around them differs,
        and a page is one block of output, so it does not flush per list.
*/
static fn host_settings_lines(host_settings address_to settings, positive which,
                              bool page)
{
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        host_setting setting;
        positive at = 0;
        positive shown = 0;
        p16 line = host_machine_hook_line(host_lists[which].hook);

        if (line)
        {
                string_format(log, page ? "  %s: %s:%p\n"
                                        : host_label "%s is %s:%p\n",
                              host_lists[which].verb, host_machine_where(),
                              (positive)line);
                if (!page)
                        log_flush();
                return;
        }

        while (host_settings_next(settings, address_of at, address_of setting))
        {
                if (setting.entry.list != host_lists[which].list)
                        continue;

                if (page && !shown)
                        string_format(log, "  %s:\n", host_lists[which].verb);

                host_settings_text(text, address_of setting);
                string_format(log, page ? "    %p  %s\n" : "%p  %s\n",
                              (positive)setting.entry.id, text);
                shown++;
        }

        if (!shown)
                string_format(log, page ? "  %s: %s\n" : host_label "%s: %s\n",
                              host_lists[which].verb, host_lists[which].empty);

        if (!page)
                log_flush();
}

static b32 host_settings_refused(string_address verb, string_address why,
                                 host_settings address_to settings)
{
        string_format(log_error, host_label "%s: %s", verb, why);
        if (string_equals(why, "the settings block is full"))
                string_format(log_error, " (%p of %p bytes); remove an entry first",
                              (positive)settings->length,
                              (positive)SPARK_SETTINGS_PAYLOAD);
        string_format(log_error, "\n");
        log_flush();
        return HOST_SETTINGS_REFUSED;
}

/*
        One settings command against a copy in memory, nothing read or written
        but that copy: CHANGED once it printed what changed and wants the
        line finished by saving, SHOWN once it printed what was asked, USAGE,
        or REFUSED once it said why.
*/
static b32 host_settings_apply(host_settings address_to settings,
                               string_address address_to arguments, positive count)
{
        string_address verb = arguments[1];
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        host_setting setting;
        string_address failed;
        positive length = 0;
        positive which = string_table_find(verb, host_lists,
                                           sizeof(host_lists[0]),
                                           array_count(host_lists));
        p16 id = 0;
        p8 list;

        if (which == array_count(host_lists))
                return HOST_SETTINGS_USAGE;

        list = host_lists[which].list;

        if (count == 2)
        {
                host_settings_lines(settings, which, false);
                return HOST_SETTINGS_SHOWN;
        }

        {
                p16 overlay = host_machine_hook_line(host_lists[which].hook);

                if (overlay)
                {
                        if (count >= 3 && string_equals(arguments[2], "mount"))
                                ;
                        else
                        {
                                host_machine_refused(verb, overlay);
                                return HOST_SETTINGS_REFUSED;
                        }
                }
        }

        for (positive at = 0; at < array_count(host_switches); at++)
        {
                p32 flag = host_switches[at].flag;

                if (!string_equals(verb, host_switches[at].verb) ||
                    !string_equals(arguments[2], host_switches[at].word))
                        continue;

                if (count == 3)
                {
                        string_format(log, host_label "%s %s %s\n", verb,
                                      host_switches[at].word,
                                      settings->flags & flag ? "off" : "on");
                        log_flush();
                        return HOST_SETTINGS_SHOWN;
                }

                if (count != 4 || (!string_equals(arguments[3], "on") &&
                                   !string_equals(arguments[3], "off")))
                        return HOST_SETTINGS_USAGE;

                if (string_equals(arguments[3], "off"))
                        settings->flags |= flag;
                else
                        settings->flags &= ~flag;

                string_format(log, host_label "%s %s %s; ", verb,
                              host_switches[at].word, arguments[3]);
                return HOST_SETTINGS_CHANGED;
        }

        if (string_equals(arguments[2], "add") || string_equals(arguments[2], "remove"))
        {
                bool adding = string_equals(arguments[2], "add");

                if (count < 4)
                        return HOST_SETTINGS_USAGE;

                if (!host_settings_words(text, sizeof(text), arguments + 3, count - 3,
                                         address_of length))
                        return host_settings_refused(verb, "an entry is 4096 bytes at most",
                                                     settings);

                if (!length)
                        return HOST_SETTINGS_USAGE;

                if (adding)
                {
                        failed = host_settings_add(settings, list, SPARK_SETTINGS_COMMAND,
                                                   text, length, address_of id);
                        if (failed)
                                return host_settings_refused(verb, failed, settings);

                        string_format(log, host_label "%s %p added: %s; ", verb,
                                      (positive)id, text);
                        return HOST_SETTINGS_CHANGED;
                }

                if (!host_settings_find(settings, list, text, address_of setting))
                {
                        string_format(log_error, host_label "%s has no entry %s\n", verb,
                                      text);
                        log_flush();
                        return HOST_SETTINGS_REFUSED;
                }

                id = setting.entry.id;
                host_settings_text(text, address_of setting);
                host_settings_drop(settings, address_of setting);
                string_format(log, host_label "%s %p removed: %s; ", verb, (positive)id,
                              text);
                return HOST_SETTINGS_CHANGED;
        }

        return HOST_SETTINGS_USAGE;
}

/*
        A command naming a file the machine will not have when the entry runs:
        a live session's files, and /root, /home and the bowls once boot stops
        mounting them, are gone at power off.
*/
static fn host_settings_note(host_settings address_to settings,
                             string_address verb, string_address text)
{
        p8 path[HOST_PATH_ROOM];
        p8 verdict[HOST_NAME_ROOM + 16];
        positive length = 0;
        bool kept = false;

        if (text[0] != '/')
                return;

        while (text[length] && text[length] != ' ' && length + 1 < sizeof(path))
        {
                path[length] = text[length];
                length++;
        }
        path[length] = end;

        if (system_access_at(AT_FDCWD, path, 0) < 0)
                return;

        host_read_text(HOST_VERDICT, verdict, sizeof(verdict));

        for (positive at = 0; at < array_count(host_kept); at++)
        {
                positive prefix = string_length(host_kept[at].path);

                kept |= host_starts(path, host_kept[at].path) && path[prefix] == '/';
        }

        if (kept && host_starts(verdict, "disk ") &&
            !(settings->flags & SPARK_SETTINGS_MOUNT_OFF))
                return;

        string_format(log, host_label "%s is read when the entry runs and is not kept "
                                      "after power off; moonwater bind %s add \"$(cat %s)\" "
                                      "keeps the script itself\n",
                      path, verb, path);
        log_flush();
}

static b32 host_settings_command(string_address address_to arguments, positive count)
{
        host_settings settings;
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        positive length = 0;
        b32 outcome;

        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");

        host_state_ready();
        host_settings_session(address_of settings);

        outcome = host_settings_apply(address_of settings, arguments, count);
        if (outcome == HOST_SETTINGS_USAGE)
                return host_usage();
        if (outcome != HOST_SETTINGS_CHANGED)
                return outcome == HOST_SETTINGS_SHOWN ? 0 : 1;

        host_settings_save(address_of settings);

        if (count > 3 && string_equals(arguments[2], "add") &&
            host_settings_words(text, sizeof(text), arguments + 3, count - 3,
                                address_of length))
                host_settings_note(address_of settings, arguments[1], text);

        return 0;
}

/* Both slots of an image not yet in place, as these settings. */
static bipolar host_settings_stamp(string_address path, host_settings address_to settings)
{
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ_WRITE | O_CLOEXEC);
        bipolar failed = 0;
        p64 offset;

        if (handle < 0)
                return handle;

        offset = host_settings_section(handle);
        if (offset)
        {
                host_settings_seal(settings);
                failed = storage_format_write(handle, (p8 address_to)settings,
                                              SPARK_SETTINGS_SLOT, offset);
                if (!failed)
                        failed = storage_format_write(handle, (p8 address_to)settings,
                                                      SPARK_SETTINGS_SLOT,
                                                      offset + SPARK_SETTINGS_SLOT);
                if (!failed)
                        failed = system_call_1(syscall(fsync), (positive)handle);
        }

        system_close(handle);
        return failed < 0 ? failed : 0;
}

/* The settings an install's own image starts with. */
static bool host_settings_install(host_install address_to install,
                                  host_settings address_to into)
{
        p8 path[HOST_PATH_ROOM];
        b32 found = -1;

        if (!host_install_refresh(install) ||
            host_mount(install->system, HOST_LOOK, "vfat", HOST_READ_ONLY) < 0)
                return false;

        if (host_join(path, sizeof(path), HOST_LOOK, HOST_IMAGE))
                found = host_settings_image(path, into);

        host_unmount(HOST_LOOK);
        return found >= 0;
}

// Events --------------------------------------------------------

/*
        init and exit, run the way a terminal runs what is typed into it:
        /shell -c, from /root, with the path a terminal gives its shell, so a
        bowl's published launchers -- pacman once bowl setup arch has run on
        a machine that keeps /bowls -- are found by name.

        init runs once per boot, as root, in the background and in the order
        written, once boot has settled the disks and somebody has answered
        its question when it asked one, so a command reads the /root and the
        bowls it will be using. Each entry's output goes to
        /run/moonwater/init/ID.log and how it ended to ID.status, the kernel
        log -- the kernel log window -- says when each starts and ends, and
        neither the prompt nor the desktop waits for any of it.

        exit runs when the machine stops, before anything is remounted
        read-only, with its output on the terminal that stopped it. Each
        entry gets ten seconds and all of them thirty; one still running then
        is killed with its process group, so a stuck command cannot keep a
        machine from powering off. A stop that is not the shell's -- the
        kernel's own orderly power off -- runs none of it.
*/
#define HOST_EVENTS_INIT HOST_STATE "/init"
#define HOST_EVENT_SHELL "/shell"
#define HOST_EXIT_EACH_NS ((p64)10000000000)
#define HOST_EXIT_ALL_NS ((p64)30000000000)
#define HOST_EVENT_POLL_NS ((p64)20000000)
#define HOST_EVENT_SHOWN 80

static string_address address_to host_event_environment(void)
{
        static string_address seed[] = {
            "TERM=dumb", "HOME=/root", "PATH=" BOWL_DEFAULT_PATH, "LANG=C.UTF-8",
            null};

        bowl_session_prepare("/root", null);
        return bowl_environment(seed);
}

static fn host_decimal(p8 address_to into, positive room, positive value)
{
        p8 digits[24];
        positive used = 0;
        positive at = 0;

        do
                digits[used++] = (p8)('0' + value % 10);
        while ((value /= 10) && used < sizeof(digits));

        while (used && at + 1 < room)
                into[at++] = digits[--used];

        into[at] = end;
}

/* A command as one short line: control bytes as spaces, and cut with ... past eighty. */
static fn host_event_shown(p8 address_to into, string_address text)
{
        positive at = 0;

        for (; text[at] && at < HOST_EVENT_SHOWN; at++)
                into[at] = (p8)text[at] < ' ' || text[at] == 0x7f ? ' ' : (p8)text[at];

        into[at] = end;
        if (text[at])
                string_append_bounded(into, "...", HOST_EVENT_SHOWN + 4);
}

static fn host_event_ending(p8 address_to into, positive room, positive status)
{
        p8 number[24];

        host_decimal(number, sizeof(number),
                     status & 0x7f ? status & 0x7f : status >> 8 & 0xff);
        string_copy_bounded(into, status & 0x7f ? "killed by signal " : "exited ", room);
        string_append_bounded(into, number, room);
}

/* One line in the kernel log, which the kernel log window shows. */
static fn host_kmsg(string_address address_to parts)
{
        p8 line[320];
        bipolar handle;

        string_copy_bounded(line, "<6>[moonwater] ", sizeof(line) - 1);
        for (; *parts; parts++)
                string_append_bounded(line, *parts, sizeof(line) - 1);
        string_append_bounded(line, "\n", sizeof(line));

        handle = system_open_at(AT_FDCWD, "/dev/kmsg", 01 | O_CLOEXEC);
        if (handle < 0)
                return;

        system_call_3(syscall(write), (positive)handle, (positive)line, string_length(line));
        system_close(handle);
}

/* /shell -c text in a session of its own, from /root, its output where asked or inherited. */
static bipolar host_event_start(string_address text, bipolar output,
                                string_address address_to environment)
{
        bipolar child = system_fork();

        if (child)
                return child;

        {
                string_address argv[] = {HOST_EVENT_SHELL, "-c", text, null};
                bipolar quiet = system_open_at(AT_FDCWD, "/dev/null",
                                               FILE_READ_WRITE | O_CLOEXEC);

                system_call(syscall(setsid));
                if (quiet > 0)
                        system_call_3(syscall(dup3), (positive)quiet, 0, 0);
                if (output > 2)
                {
                        system_call_3(syscall(dup3), (positive)output, 1, 0);
                        system_call_3(syscall(dup3), (positive)output, 2, 0);
                }

                system_call_1(syscall(chdir), (positive)(string_address)"/root");
                (void)shell_exec_file(HOST_EVENT_SHELL, argv, 3, environment);
                system_call_1(syscall(exit), 127);
        }

        return -1;
}

/*
        A child to its end, or past limit killed with its group and then
        waited for: its wait status, and whether it had to be stopped. A limit
        of zero waits as long as it takes.
*/
static positive host_event_wait(bipolar child, p64 limit, bool address_to stopped)
{
        p64 started = system_clock_ns(HOST_CLOCK_BOOTTIME);
        positive status = 0;

        address_to stopped = false;

        for (;;)
        {
                bipolar reaped = system_call_4(syscall(wait4), (positive)child,
                                               (positive)address_of status,
                                               limit ? 1 : 0, 0);

                if (reaped == child)
                        return status;
                if (reaped == -4)
                        continue;
                if (reaped < 0)
                        return 0;

                if (system_clock_ns(HOST_CLOCK_BOOTTIME) - started >= limit)
                {
                        system_call_2(syscall(kill), (positive)(-child), SIGKILL);
                        system_call_2(syscall(kill), (positive)child, SIGKILL);
                        address_to stopped = true;
                        limit = 0;
                        continue;
                }

                host_pause(HOST_EVENT_POLL_NS);
        }
}

static fn host_events_boot(host_settings address_to settings)
{
        host_setting setting;
        positive at = 0;

        if (!settings || !host_settings_count(settings, SPARK_SETTINGS_INIT) ||
            host_machine_hook_line(MOONWATER_HOOK_INIT) || system_fork())
                return;

        //      The runner, from here on: its own session, outliving boot.
        system_call(syscall(setsid));
        system_make_directory_at(AT_FDCWD, HOST_EVENTS_INIT, 0700);

        for (;;)
        {
                p8 verdict[HOST_NAME_ROOM + 16];

                if (host_read_text(HOST_VERDICT, verdict, sizeof(verdict)) >= 0
                        ? !host_starts(verdict, "ask ")
                        : system_clock_ns(HOST_CLOCK_BOOTTIME) >= HOST_VERDICT_WAIT_NS)
                        break;

                host_pause(HOST_POLL_NS * 2);
        }

        while (host_settings_next(settings, address_of at, address_of setting))
        {
                p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
                p8 shown[HOST_EVENT_SHOWN + 4];
                p8 id[8];
                p8 path[HOST_PATH_ROOM];
                p8 status_path[HOST_PATH_ROOM];
                p8 ending[48];
                bipolar output;
                bipolar child;
                positive status;
                bool stopped = false;

                if (setting.entry.list != SPARK_SETTINGS_INIT)
                        continue;

                host_settings_text(text, address_of setting);
                host_event_shown(shown, text);
                host_decimal(id, sizeof(id), setting.entry.id);

                if (!host_join(path, sizeof(path), HOST_EVENTS_INIT "/", id) ||
                    !host_join(status_path, sizeof(status_path), path, ".status") ||
                    !host_join(path, sizeof(path), path, ".log"))
                        continue;

                {
                        string_address line[] = {"init ", id, " started: ", shown, null};

                        host_kmsg(line);
                }

                host_write_text(status_path, "running\n");
                output = system_open_at_mode(AT_FDCWD, path, FILE_WRITE | O_CLOEXEC, 0600);
                child = host_event_start(text, output, host_event_environment());
                if (output >= 0)
                        system_close(output);

                status = child < 0 ? (positive)127 << 8
                                   : host_event_wait(child, 0, address_of stopped);
                host_event_ending(ending, sizeof(ending), status);
                string_append_bounded(ending, "\n", sizeof(ending));
                host_write_text(status_path, ending);
                ending[string_length(ending) - 1] = end;

                {
                        string_address line[] = {"init ", id, " ", ending, "; its output is in ",
                                                 path, null};

                        host_kmsg(line);
                }
        }

        system_call_1(syscall(exit), 0);
}

fn host_exit_run(void)
{
        host_settings settings;
        host_setting setting;
        positive at = 0;
        p64 started = system_clock_ns(HOST_CLOCK_BOOTTIME);

        bool machine = host_machine_stop();

        if (!bowl_is_root() ||
            (!host_settings_kept(address_of settings) &&
             !host_settings_booted(address_of settings)))
                return;

        if (machine && host_machine_hook_line(MOONWATER_HOOK_END))
                return;

        while (host_settings_next(address_of settings, address_of at, address_of setting))
        {
                p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
                p8 ending[48];
                p64 spent = system_clock_ns(HOST_CLOCK_BOOTTIME) - started;
                p64 limit = HOST_EXIT_ALL_NS - spent;
                bool stopped = false;
                positive status;
                bipolar child;

                if (setting.entry.list != SPARK_SETTINGS_EXIT)
                        continue;

                if (spent >= HOST_EXIT_ALL_NS)
                {
                        string_format(log_error, host_label "exit: thirty seconds are spent, "
                                                            "and the rest do not run\n");
                        log_flush();
                        break;
                }

                host_settings_text(text, address_of setting);
                string_format(log, host_label "exit %p: %s\n", (positive)setting.entry.id, text);
                log_flush();

                //      The environment init's entries get, not the stopping
                //      shell's: a bound poweroff has almost none.
                child = host_event_start(text, -1, host_event_environment());
                if (child < 0)
                        continue;

                status = host_event_wait(child, limit < HOST_EXIT_EACH_NS ? limit
                                                                          : HOST_EXIT_EACH_NS,
                                         address_of stopped);
                host_event_ending(ending, sizeof(ending), status);

                if (stopped)
                        string_format(log_error, host_label "exit %p did not finish in time "
                                                            "and was killed\n",
                                      (positive)setting.entry.id);
                else if (status)
                        string_format(log_error, host_label "exit %p %s\n",
                                      (positive)setting.entry.id, ending);
                log_flush();
        }
}

/*
        Canvas, off and on.

        Reading needs nothing; the kernel decides who may turn it off or on.
        Off is usually typed into a Canvas terminal, which closes under it, so
        the way back is said first and the hangup that closing sends is
        ignored. Where the kernel console is not a screen -- console=ttyS0,
        with init's shell on the serial line -- the text console off leaves
        would have no shell, so one is started on tty1.
*/
static bipolar host_canvas_request(positive request,
                                   struct canvas_control address_to control)
{
        memory_zero(control, sizeof(address_to control));
        control->request = (unsigned int)request;
        return host_spark_once(SPARK_IOCTL_CANVAS, control, FILE_READ);
}

static fn host_canvas_write(string_address prefix,
                            struct canvas_control address_to control)
{
        if (!control->running)
        {
                string_format(log, "%sCanvas is off; moonwater canvas on starts it\n",
                              prefix);
                return;
        }

        string_format(log, "%sCanvas is on: %s, %p window%s\n", prefix,
                      (string_address)control->driver, (positive)control->windows,
                      control->windows == 1 ? "" : "s");

        for (positive at = 0; at < control->output_count && at < SPARK_CANVAS_OUTPUTS; at++)
                string_format(log, "%s  %s: %p by %p, %p Hz\n", prefix,
                              (string_address)control->output[at].connector,
                              (positive)control->output[at].width,
                              (positive)control->output[at].height,
                              (positive)control->output[at].refresh);

        if (control->detached)
                string_format(log, "%s%p window%s still open off the desktop\n", prefix,
                              (positive)control->detached,
                              control->detached == 1 ? "" : "s");

        if (control->suspended)
                string_format(log, "%sanother program holds the display; "
                                   "Canvas ignores input until it lets go\n",
                              prefix);
}

static fn host_canvas_say(struct canvas_control address_to control)
{
        host_canvas_write(host_label, control);
        log_flush();
}

/* Whether the kernel console is a screen, where init's shell is on tty1. */
static bool host_console_is_screen(void)
{
        p8 active[128];

        if (host_read_text("/sys/class/tty/console/active", active, sizeof(active)) <= 0)
                return true;

        for (string_address at = (string_address)active; *at;)
        {
                if (at[0] == 't' && at[1] == 't' && at[2] == 'y' &&
                    at[3] >= '0' && at[3] <= '9')
                        return true;

                while (*at && *at != ' ')
                        at++;
                while (*at == ' ')
                        at++;
        }

        return false;
}

/* A shell on tty1, in a session of its own, for a console that has none. */
static fn host_tty1_shell(void)
{
        bipolar child = system_fork();

        if (child)
                return;

        {
                string_address argv[] = {HOST_EVENT_SHELL, null};
                bipolar tty;

                system_call(syscall(setsid));
                tty = system_open_at(AT_FDCWD, "/dev/tty1", FILE_READ_WRITE);
                if (tty < 3)
                        system_call_1(syscall(exit), 126);

                for (positive at = 0; at < 3; at++)
                        system_call_3(syscall(dup3), (positive)tty, at, 0);

                system_call_1(syscall(chdir), (positive)(string_address)"/root");
                (void)shell_exec_file(HOST_EVENT_SHELL, argv, 1, host_event_environment());
                system_call_1(syscall(exit), 127);
        }
}

/* moonwater canvas [on|off] */
static b32 host_canvas(string_address address_to arguments, positive count)
{
        struct canvas_control control;
        bipolar failed;

        if (count > 3)
                return host_usage();

        if (count < 3)
        {
                failed = host_canvas_request(SPARK_CANVAS_STATUS, address_of control);
                if (failed < 0)
                        return host_fail(SPARK_DEVICE, failed);

                host_canvas_say(address_of control);
                return 0;
        }

        if (string_equals(arguments[2], "off"))
        {
                string_format(log, host_label "Canvas off: every window closes, this one too. "
                                              "On the text console, moonwater canvas on "
                                              "brings the desktop back.\n");
                log_flush();

                system_signal_install(1, 1, 0, 0, null);

                failed = host_canvas_request(SPARK_CANVAS_OFF, address_of control);
                if (failed == -EPERM)
                        return host_refuse("turning Canvas off needs root (CAP_SYS_ADMIN)%s\n", "");
                if (failed == -EALREADY)
                        return host_refuse("Canvas is already off%s\n", "");
                if (failed < 0)
                        return host_fail("canvas off", failed);

                if (!host_console_is_screen())
                        host_tty1_shell();

                return 0;
        }

        if (string_equals(arguments[2], "on"))
        {
                failed = host_canvas_request(SPARK_CANVAS_ON, address_of control);
                if (failed == -EPERM)
                        return host_refuse("turning Canvas on needs root (CAP_SYS_ADMIN)%s\n", "");
                if (failed == -EALREADY)
                        return host_refuse("Canvas is already on%s\n", "");
                if (failed == -EBUSY)
                {
                        if (control.master_command[0])
                                string_format(log_error, host_label "%s (pid %p) holds the display; "
                                                                    "Canvas stays off until it lets go\n",
                                              (string_address)control.master_command,
                                              (positive)control.master_pid);
                        else
                                string_format(log_error, host_label "another program holds the display; "
                                                                    "Canvas stays off until it lets go\n");
                        log_flush();
                        return 1;
                }
                if (failed == -ENODEV)
                        return host_refuse("there is no display for Canvas to start on%s\n", "");
                if (failed < 0)
                        return host_fail("canvas on", failed);

                host_canvas_say(address_of control);
                return 0;
        }

        return host_usage();
}

/*      ----------------------------------------------------------------
        radio: wifi and bluetooth, and the nl80211 they ask the kernel through.
        ---------------------------------------------------------------- */

/*
        Wireless and bluetooth, as moonwater verbs.

        Secrets stay on /root so an image update does not take the password
        with it. /ip watch still owns the address: this only joins the radio
        and says which link to prefer when both have carrier.
*/

/*      ----------------------------------------------------------------
        nl80211: how the wifi above talks to the kernel.
        ---------------------------------------------------------------- */

/*
        Experimental C standard library

        nl80211: join a station, leave it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_NL80211
#define STANDARD_MODERN_C_NET_NL80211

#define GENL_ID_CTRL 16
#define GENL_HEADER 4
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_MCAST_GROUPS 7
#define CTRL_ATTR_MCAST_GRP_NAME 1
#define CTRL_ATTR_MCAST_GRP_ID 2

#define NL80211_CMD_GET_WIPHY 1
#define NL80211_CMD_NEW_WIPHY 3
#define NL80211_CMD_GET_INTERFACE 5
#define NL80211_CMD_NEW_INTERFACE 7
#define NL80211_CMD_NEW_KEY 11
#define NL80211_CMD_GET_STATION 17
#define NL80211_CMD_SET_STATION 18
#define NL80211_CMD_NEW_STATION 19
#define NL80211_CMD_CONNECT 46
#define NL80211_CMD_DISCONNECT 48

#define NL80211_ATTR_WIPHY 1
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_IFNAME 4
#define NL80211_ATTR_IFTYPE 5
#define NL80211_ATTR_MAC 6
#define NL80211_ATTR_KEY_DATA 7
#define NL80211_ATTR_KEY_IDX 8
#define NL80211_ATTR_KEY_CIPHER 9
#define NL80211_ATTR_KEY_SEQ 10
#define NL80211_ATTR_KEY_DEFAULT 11
#define NL80211_ATTR_SSID 52
#define NL80211_ATTR_AUTH_TYPE 53
#define NL80211_ATTR_KEY_TYPE 55
#define NL80211_ATTR_TIMED_OUT 65
#define NL80211_ATTR_STA_FLAGS2 67
#define NL80211_ATTR_CONTROL_PORT 68
#define NL80211_ATTR_PRIVACY 70
#define NL80211_ATTR_STATUS_CODE 72
#define NL80211_ATTR_CIPHER_SUITES_PAIRWISE 73
#define NL80211_ATTR_CIPHER_SUITE_GROUP 74
#define NL80211_ATTR_WPA_VERSIONS 75
#define NL80211_ATTR_AKM_SUITES 76
#define NL80211_ATTR_REQ_IE 77
#define NL80211_ATTR_EXT_FEATURES 217
#define NL80211_ATTR_PMK 254

#define NL80211_IFTYPE_STATION 2
#define NL80211_AUTHTYPE_OPEN 0
#define NL80211_WPA_VERSION_2 2
#define NL80211_KEYTYPE_GROUP 0
#define NL80211_KEYTYPE_PAIRWISE 1
#define NL80211_STA_FLAG_AUTHORIZED 1
#define NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK 15
#define WLAN_CIPHER_CCMP 0x000fac04u
#define WLAN_AKM_PSK 0x000fac02u
#define NL80211_CONNECT_SECONDS 20
#define WIFI_EAPOL_SECONDS 8
#define WIFI_EAPOL_HDR 99
#define WIFI_GTK_WRAP 24
#define WIFI_WRAP_MOST 408

typedef struct
{
        b32 handle;
        p16 family;
        p32 mlme;
} nl80211;

typedef struct
{
        p32 index;
        p32 wiphy;
        bool found;
        bool has_mac;
        p8 mac[6];
        p8 name[IFNAME_SIZE];
} nl80211_iface;

typedef struct
{
        p32 wiphy;
        p32 seen;
        bool offload;
} nl80211_wiphy_query;

static COLD bipolar nl80211_disconnect(nl80211 address_to session, p32 index);

typedef struct
{
        p16 family;
        p32 mlme;
} nl80211_family_info;

static COLD bool nl80211_begin(netlink_buffer address_to buffer, p16 family, p8 command,
                          p16 flags, p32 sequence)
{
        p8 address_to body;

        if (!netlink_begin(buffer, family, flags, sequence, GENL_HEADER))
                return false;

        body = (p8 address_to)netlink_body(buffer);
        body[0] = command;
        body[1] = 1;
        body[2] = 0;
        body[3] = 0;
        return true;
}

static COLD bool nl80211_attribute_u32(netlink_buffer address_to buffer, p16 type,
                                  p32 value)
{
        return netlink_attribute_add(buffer, type, address_of value, 4);
}

static COLD p32 nl80211_find_u32(netlink_header address_to header, p16 type, p32 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 4)
                return missing;
        return memory_load_unaligned(p32, at);
}

static COLD p16 nl80211_find_u16(netlink_header address_to header, p16 type,
                            p16 missing)
{
        positive size = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER, type,
                                                       address_of size);

        if (!at || size < 2)
                return missing;
        return (p16)memory_load_unaligned(p16, at);
}

static COLD bool nl80211_attr(netlink_header address_to header, p16 type)
{
        return netlink_find(header, GENL_HEADER, type, null) != null;
}

static COLD bool nl80211_family_seen(netlink_header address_to header,
                                address_any context)
{
        nl80211_family_info address_to info = (nl80211_family_info address_to)context;
        positive size = 0;
        positive groups_length = 0;
        p8 address_to at = (p8 address_to)netlink_find(header, GENL_HEADER,
                                                       CTRL_ATTR_FAMILY_ID,
                                                       address_of size);
        p8 address_to groups;
        positive cursor = 0;

        if (at && size >= 2)
                info->family = memory_load_unaligned(p16, at);

        groups = (p8 address_to)netlink_find(header, GENL_HEADER,
                                             CTRL_ATTR_MCAST_GROUPS,
                                             address_of groups_length);
        while (groups && cursor + sizeof(netlink_attribute) <= groups_length)
        {
                netlink_attribute address_to attribute =
                    (netlink_attribute address_to)(groups + cursor);
                positive payload;
                p8 address_to name;
                p8 address_to id;
                positive name_length = 0;
                positive id_length = 0;

                if (attribute->length < sizeof(netlink_attribute) ||
                    cursor + attribute->length > groups_length)
                        break;
                payload = attribute->length - sizeof(netlink_attribute);
                name = (p8 address_to)netlink_find_span(
                    groups + cursor + sizeof(netlink_attribute), payload,
                    CTRL_ATTR_MCAST_GRP_NAME, address_of name_length);
                id = (p8 address_to)netlink_find_span(
                    groups + cursor + sizeof(netlink_attribute), payload,
                    CTRL_ATTR_MCAST_GRP_ID, address_of id_length);
                if (name && name_length >= 4 && id && id_length >= 4 &&
                    !memory_compare(name, "mlme", 4))
                        info->mlme = memory_load_unaligned(p32, id);
                cursor += netlink_align(attribute->length);
        }

        return false;
}

static COLD bipolar nl80211_open(nl80211 address_to session)
{
        netlink_buffer request = {0};
        p32 sequence;
        p8 name[] = "nl80211";
        bipolar handle;
        nl80211_family_info info = {0};

        memory_fill(session, 0, sizeof(*session));
        session->handle = -1;

        handle = netlink_open_protocol(NETLINK_GENERIC, 0);
        if (handle < 0)
                return handle;

        sequence = netlink_sequence_take();
        if (!nl80211_begin(address_of request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
                           NLM_REQUEST | NLM_ACK, sequence))
        {
                socket_close((b32)handle);
                return -1;
        }

        netlink_attribute_add(address_of request, CTRL_ATTR_FAMILY_NAME, name,
                              sizeof(name));

        if (netlink_transact((b32)handle, address_of request, sequence,
                             nl80211_family_seen, address_of info) < 0 ||
            !info.family)
        {
                socket_close((b32)handle);
                return -19;
        }

        if (info.mlme &&
            socket_option_set((b32)handle, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                              address_of info.mlme, sizeof(info.mlme)) >= 0)
                session->mlme = info.mlme;

        session->handle = (b32)handle;
        session->family = info.family;
        return 0;
}

static COLD fn nl80211_close(nl80211 address_to session)
{
        if (session->handle >= 0)
                socket_close(session->handle);
        session->handle = -1;
}

static COLD bool nl80211_iface_seen(netlink_header address_to header,
                               address_any context)
{
        nl80211_iface address_to found = (nl80211_iface address_to)context;
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        p32 type;
        p32 index;
        positive length = 0;
        string_address name;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_INTERFACE &&
            body[0] != NL80211_CMD_GET_INTERFACE)
                return true;

        type = nl80211_find_u32(header, NL80211_ATTR_IFTYPE, 0);
        if (found->found && type != NL80211_IFTYPE_STATION)
                return true;

        index = nl80211_find_u32(header, NL80211_ATTR_IFINDEX, 0);
        if (!index)
                return true;

        name = (string_address)netlink_find(header, GENL_HEADER,
                                            NL80211_ATTR_IFNAME,
                                            address_of length);
        found->index = index;
        found->found = true;
        found->wiphy = nl80211_find_u32(header, NL80211_ATTR_WIPHY, 0);
        found->name[0] = end;
        if (name && length)
        {
                if (length >= IFNAME_SIZE)
                        length = IFNAME_SIZE - 1;
                memory_copy(found->name, name, length);
                found->name[length] = end;
        }

        {
                positive mac_length = 0;
                p8 address_to mac = (p8 address_to)netlink_find(
                    header, GENL_HEADER, NL80211_ATTR_MAC, address_of mac_length);

                if (mac && mac_length >= 6)
                {
                        memory_copy(found->mac, mac, 6);
                        found->has_mac = true;
                }
        }

        return type != NL80211_IFTYPE_STATION;
}

static COLD bipolar nl80211_interface(nl80211 address_to session,
                                 nl80211_iface address_to found)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        memory_fill(found, 0, sizeof(*found));
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_INTERFACE,
                           NLM_REQUEST | NLM_DUMP, sequence))
                return -1;

        return netlink_transact(session->handle, address_of request, sequence,
                                nl80211_iface_seen, found) < 0
                   ? -1
                   : (found->found ? 0 : -19);
}

/* WPA's PBKDF2 and PRF are HMAC-SHA-1, which is crypto.c's HMAC under a
   different digest and nothing else. */
static COLD fn wifi_hmac_sha1(p8 address_to key, positive key_length,
                         p8 address_to data, positive length, p8 address_to out)
{
        crypto_mac mac;

        crypto_hmac_open(address_of mac, DIGEST_SHA1, 20, key, key_length);
        crypto_hmac_write(address_of mac, data, length);
        crypto_hmac_close(address_of mac, out);
}

static COLD p8 wifi_nibble(p8 byte)
{
        if (byte_is_digit(byte))
                return (p8)(byte - '0');
        byte = byte_to_lower(byte);
        return (p8)(byte - 'a' + 10);
}

static COLD bool wifi_psk(p8 address_to ssid, positive ssid_length,
                     p8 address_to pass, positive pass_length, p8 address_to pmk)
{
        p8 block[36];
        p8 last[20];
        p8 mix[20];
        positive round;
        positive which;
        positive i;

        if (pass_length == 64)
        {
                for (i = 0; i < 64; i++)
                        if (!byte_is_hexadecimal(pass[i]))
                                return false;
                for (i = 0; i < 32; i++)
                        pmk[i] = (p8)((wifi_nibble(pass[i * 2]) << 4) |
                                      wifi_nibble(pass[i * 2 + 1]));
                return true;
        }

        if (pass_length < 8 || pass_length > 63 || !ssid_length ||
            ssid_length > 32)
                return false;

        memory_copy(block, ssid, ssid_length);
        for (which = 1; which <= 2; which++)
        {
                block[ssid_length] = 0;
                block[ssid_length + 1] = 0;
                block[ssid_length + 2] = 0;
                block[ssid_length + 3] = (p8)which;
                wifi_hmac_sha1(pass, pass_length, block, ssid_length + 4, last);
                memory_copy(mix, last, 20);
                for (round = 1; round < 4096; round++)
                {
                        wifi_hmac_sha1(pass, pass_length, last, 20, last);
                        for (i = 0; i < 20; i++)
                                mix[i] ^= last[i];
                }
                memory_copy(pmk + (which - 1) * 20, mix,
                            which == 1 ? 20 : 12);
        }

        crypto_forget(block, sizeof(block));
        crypto_forget(last, sizeof(last));
        crypto_forget(mix, sizeof(mix));
        return true;
}

/* One byte of InvMixColumns.  The matrix's four rows are the same four
   coefficients rotated, so the row it is wanted for says where to start
   reading them, and the multiply is crypto.c's constant-time one -- the
   same GF(2^8) its S-box inversion runs in. */
static COLD p8 wifi_unmix(p8 address_to column, positive row)
{
        static const p8 factor[4] = {0x0e, 0x0b, 0x0d, 0x09};
        p8 mixed = 0;

        for (positive at = 0; at < 4; at++)
                mixed ^= crypto_aes_field_multiply(
                    column[at], factor[(4 + at - row) & 3]);

        return mixed;
}

/* The inverse S-box, built rather than written out: it is the forward box's
   inverse permutation, and crypto.c already computes that box in the field
   it lives in. Two hundred and fifty six hand-typed bytes are two hundred
   and fifty six chances to mistype one, and built this way the two boxes
   cannot disagree. The unwrap builds it once and lends it to every block. */
static COLD fn wifi_inverse_box(p8 address_to inverse)
{
        for (positive at = 0; at < 256; at++)
                inverse[crypto_aes_substitute((p8)at)] = (p8)at;
}

static COLD fn wifi_aes_decrypt(p8 address_to key, p8 address_to in,
                           p8 address_to out, const p8 address_to inverse)
{
        p8 round[176];
        p8 state[16];
        p8 hold[16];
        positive step;
        positive row;
        p8 temp;

        crypto_aes128_expand(key, round);
        memory_copy(state, in, 16);
        for (step = 0; step < 16; step++)
                state[step] ^= round[160 + step];

        /*
                The last round is every other round without InvMixColumns, so
                the loop runs once more and leaves from the middle rather
                than repeating its first three steps underneath itself.
        */
        for (step = 9;; step--)
        {
                //      InvShiftRows: row r of the state -- the bytes r, r+4,
                //      r+8 and r+12 -- rotates right by r.
                for (row = 1; row < 4; row++)
                        for (positive turn = 0; turn < row; turn++)
                        {
                                temp = state[row + 12];
                                state[row + 12] = state[row + 8];
                                state[row + 8] = state[row + 4];
                                state[row + 4] = state[row];
                                state[row] = temp;
                        }

                for (row = 0; row < 16; row++)
                        state[row] = inverse[state[row]];
                for (row = 0; row < 16; row++)
                        state[row] ^= round[step * 16 + row];
                if (!step)
                        break;

                memory_copy(hold, state, 16);
                for (row = 0; row < 4; row++)
                        for (positive at = 0; at < 4; at++)
                                state[row * 4 + at] =
                                    wifi_unmix(hold + row * 4, at);
        }

        memory_copy(out, state, 16);
        crypto_forget(round, sizeof(round));
        crypto_forget(state, sizeof(state));
        crypto_forget(hold, sizeof(hold));
}

static COLD bool wifi_kw_unwrap(p8 address_to kek, p8 address_to wrap, positive length,
                           p8 address_to plain, positive address_to plain_length)
{
        p8 block[16];
        p8 a[8];
        p8 inverse[256];
        p8 r[WIFI_WRAP_MOST - 8];
        positive words;
        positive round;
        positive i;
        p64 t;

        if (length < 24 || (length & 7) || length > WIFI_WRAP_MOST)
                return false;
        words = (length / 8) - 1;
        wifi_inverse_box(inverse);
        memory_copy(a, wrap, 8);
        memory_copy(r, wrap + 8, words * 8);
        for (round = 6; round > 0; round--)
                for (i = words; i > 0; i--)
                {
                        t = (p64)words * (round - 1) + i;
                        crypto_put_be64(a, crypto_be64(a) ^ t);
                        memory_copy(block, a, 8);
                        memory_copy(block + 8, r + (i - 1) * 8, 8);
                        wifi_aes_decrypt(kek, block, block, inverse);
                        memory_copy(a, block, 8);
                        memory_copy(r + (i - 1) * 8, block + 8, 8);
                }

        if (crypto_be64(a) != 0xa6a6a6a6a6a6a6a6ull)
                return false;
        memory_copy(plain, r, words * 8);
        address_to plain_length = words * 8;
        crypto_forget(block, sizeof(block));
        crypto_forget(a, sizeof(a));
        crypto_forget(r, sizeof(r));
        return true;
}

static COLD fn wifi_ptk(p8 address_to pmk, p8 address_to ap, p8 address_to sta,
                   p8 address_to anonce, p8 address_to snonce, p8 address_to ptk)
{
        p8 label[] = "Pairwise key expansion";
        p8 data[6 + 6 + 32 + 32];
        p8 input[22 + 1 + 76 + 1];
        p8 hash[20];
        p8 address_to min_mac;
        p8 address_to max_mac;
        p8 address_to min_nonce;
        p8 address_to max_nonce;
        positive used = 0;
        positive which;

        if (memory_compare(ap, sta, 6) < 0)
        {
                min_mac = ap;
                max_mac = sta;
        }
        else
        {
                min_mac = sta;
                max_mac = ap;
        }
        if (memory_compare(anonce, snonce, 32) < 0)
        {
                min_nonce = anonce;
                max_nonce = snonce;
        }
        else
        {
                min_nonce = snonce;
                max_nonce = anonce;
        }

        memory_copy(data, min_mac, 6);
        memory_copy(data + 6, max_mac, 6);
        memory_copy(data + 12, min_nonce, 32);
        memory_copy(data + 44, max_nonce, 32);
        memory_copy(input, label, 22);
        input[22] = 0;
        memory_copy(input + 23, data, 76);
        for (which = 0; used < 64; which++)
        {
                input[99] = (p8)which;
                wifi_hmac_sha1(pmk, 32, input, 100, hash);
                memory_copy(ptk + used, hash, used + 20 > 64 ? 64 - used : 20);
                used += 20;
        }
        crypto_forget(data, sizeof(data));
        crypto_forget(input, sizeof(input));
        crypto_forget(hash, sizeof(hash));
}

static const p8 wifi_rsn_ie[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
                                 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};

static COLD bool nl80211_ext_bit(p8 address_to bits, positive length, positive which)
{
        return which / 8 < length && (bits[which / 8] & (1u << (which % 8)));
}

static COLD bool nl80211_wiphy_seen(netlink_header address_to header, address_any context)
{
        nl80211_wiphy_query address_to query = (nl80211_wiphy_query address_to)context;
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        positive size = 0;
        p8 address_to bits;
        p32 wiphy;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_WIPHY && body[0] != NL80211_CMD_GET_WIPHY)
                return true;

        wiphy = nl80211_find_u32(header, NL80211_ATTR_WIPHY, query->seen);
        if (nl80211_attr(header, NL80211_ATTR_WIPHY))
                query->seen = wiphy;
        if (wiphy != query->wiphy)
                return true;

        bits = (p8 address_to)netlink_find(
            header, GENL_HEADER, NL80211_ATTR_EXT_FEATURES, address_of size);
        if (bits && nl80211_ext_bit(bits, size,
                                    NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK))
                query->offload = true;
        return true;
}

static COLD bool nl80211_psk_offload(nl80211 address_to session, p32 wiphy)
{
        netlink_buffer request = {0};
        p32 sequence;
        nl80211_wiphy_query query = {.wiphy = wiphy, .seen = ~0u};

        sequence = netlink_sequence_take();
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_WIPHY, NLM_REQUEST | NLM_DUMP,
                           sequence))
                return false;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_WIPHY, wiphy);
        if (netlink_transact(session->handle, address_of request, sequence,
                             nl80211_wiphy_seen, address_of query) < 0)
                return false;
        return query.offload;
}

static COLD bool nl80211_station_seen(netlink_header address_to header,
                                 address_any context)
{
        p8 address_to body = (p8 address_to)header + NETLINK_HEADER;
        p8 address_to into = (p8 address_to)context;
        positive length = 0;
        p8 address_to mac;

        if (header->length < NETLINK_HEADER + GENL_HEADER)
                return true;
        if (body[0] != NL80211_CMD_NEW_STATION &&
            body[0] != NL80211_CMD_GET_STATION)
                return true;
        mac = (p8 address_to)netlink_find(header, GENL_HEADER, NL80211_ATTR_MAC,
                                          address_of length);
        if (mac && length >= 6)
        {
                memory_copy(into, mac, 6);
                return false;
        }
        return true;
}

static COLD bool nl80211_station(nl80211 address_to session, p32 index, p8 address_to mac)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        memory_fill(mac, 0, 6);
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_GET_STATION, NLM_REQUEST | NLM_DUMP,
                           sequence))
                return false;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        if (netlink_transact(session->handle, address_of request, sequence,
                             nl80211_station_seen, mac) < 0)
                return false;
        return mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5];
}

static COLD bipolar nl80211_new_key(nl80211 address_to session, p32 index, p8 idx,
                               p32 type, p8 address_to mac, p8 address_to key,
                               positive key_length, p8 address_to seq,
                               positive seq_length, bool group_default)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 ccmp = WLAN_CIPHER_CCMP;

        if (!nl80211_begin(address_of request, session->family, NL80211_CMD_NEW_KEY,
                           NLM_REQUEST | NLM_ACK, sequence))
                return -1;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_KEY_DATA, key,
                              key_length);
        netlink_attribute_add(address_of request, NL80211_ATTR_KEY_IDX, address_of idx,
                              1);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_KEY_CIPHER, ccmp);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_KEY_TYPE, type);
        if (mac)
                netlink_attribute_add(address_of request, NL80211_ATTR_MAC, mac, 6);
        if (seq && seq_length)
                netlink_attribute_add(address_of request, NL80211_ATTR_KEY_SEQ, seq,
                                      seq_length);
        if (group_default)
                netlink_attribute_add(address_of request, NL80211_ATTR_KEY_DEFAULT,
                                      null, 0);
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

static COLD bipolar nl80211_authorize(nl80211 address_to session, p32 index,
                                 p8 address_to mac)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 flags[2];

        flags[0] = (p32)1 << NL80211_STA_FLAG_AUTHORIZED;
        flags[1] = flags[0];
        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_SET_STATION, NLM_REQUEST | NLM_ACK,
                           sequence))
                return -1;
        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_MAC, mac, 6);
        netlink_attribute_add(address_of request, NL80211_ATTR_STA_FLAGS2, flags,
                              sizeof(flags));
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

static COLD bipolar nl80211_eapol_open(p32 index)
{
        socket_address_packet self = {
            .family = AF_PACKET,
            .protocol = network_order_16(ETH_P_PAE),
            .index = index,
        };
        bipolar handle = socket_new(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC,
                                    (b32)network_order_16(ETH_P_PAE));

        if (handle < 0)
                return handle;
        if (socket_bind((b32)handle, address_of self, sizeof(self)) < 0)
        {
                socket_close((b32)handle);
                return -1;
        }
        return handle;
}

static COLD fn wifi_eapol_mic(p8 address_to kck, p8 address_to frame, positive length)
{
        p8 hash[20];
        p8 saved[16];

        memory_copy(saved, frame + 81, 16);
        memory_fill(frame + 81, 0, 16);
        wifi_hmac_sha1(kck, 16, frame, length, hash);
        memory_copy(frame + 81, hash, 16);
        crypto_forget(hash, sizeof(hash));
        crypto_forget(saved, sizeof(saved));
}

static COLD bipolar wifi_eapol_send(b32 handle, p32 index, p8 address_to bssid,
                               p8 address_to frame, positive length)
{
        socket_address_packet to = {.family = AF_PACKET,
                                    .protocol = network_order_16(ETH_P_PAE),
                                    .index = index,
                                    .halen = 6};

        memory_copy(to.addr, bssid, 6);
        return socket_send(handle, frame, length, 0, address_of to, sizeof(to)) ==
                       (bipolar)length
                   ? 0
                   : -1;
}

static COLD bipolar wifi_handshake(nl80211 address_to session, p32 index, b32 eapol,
                              p8 address_to sta, p8 address_to bssid,
                              p8 address_to pmk)
{
        p8 snonce[32];
        p8 anonce[32];
        p8 ptk[64];
        p8 gtk[32];
        p8 rsc[8];
        p8 frame[512];
        p8 replay[8];
        socket_address_packet from;
        network_deadline deadline;
        positive gtk_length = 0;
        p8 gtk_idx = 1;
        bool have_anonce = false;
        bipolar got;

        if (system_random_fill(snonce, sizeof(snonce), 0) < 0)
                return -1;
        if (!network_deadline_begin(address_of deadline, WIFI_EAPOL_SECONDS, 0))
        {
                crypto_forget(snonce, sizeof(snonce));
                return -1;
        }

        while (network_wait_readable_until(eapol, address_of deadline) > 0)
        {
                p32 from_length = sizeof(from);
                p16 info;
                positive length;
                positive data_length;
                p8 address_to data;

                memory_fill(address_of from, 0, sizeof(from));
                got = socket_receive(eapol, frame, sizeof(frame), 0, address_of from,
                                     address_of from_length);
                if (got < WIFI_EAPOL_HDR)
                        continue;
                length = (positive)got;
                if (frame[1] != 3 || frame[4] != 2)
                        continue;
                info = network_load_16(frame + 5);
                if ((info & 7) != 2 || !(info & 8))
                        continue;
                if (!(info & 0x80))
                        continue;

                if (!(info & 0x100))
                {
                        memory_copy(anonce, frame + 17, 32);
                        memory_copy(replay, frame + 9, 8);
                        have_anonce = true;
                        wifi_ptk(pmk, bssid, sta, anonce, snonce, ptk);
                        memory_fill(frame, 0, WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        frame[0] = 1;
                        frame[1] = 3;
                        network_store_16(frame + 2, (p16)(95 + sizeof(wifi_rsn_ie)));
                        frame[4] = 2;
                        network_store_16(frame + 5, 0x010a);
                        network_store_16(frame + 7, 16);
                        memory_copy(frame + 9, replay, 8);
                        memory_copy(frame + 17, snonce, 32);
                        network_store_16(frame + 97, (p16)sizeof(wifi_rsn_ie));
                        memory_copy(frame + 99, wifi_rsn_ie, sizeof(wifi_rsn_ie));
                        wifi_eapol_mic(ptk, frame, WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        wifi_eapol_send(eapol, index, bssid, frame,
                                        WIFI_EAPOL_HDR + sizeof(wifi_rsn_ie));
                        continue;
                }

                if (!have_anonce || !(info & 0x1000))
                        continue;

                data_length = network_load_16(frame + 97);
                if (WIFI_EAPOL_HDR + data_length > length)
                        continue;
                {
                        p8 mic[16];
                        p8 check[20];

                        memory_copy(mic, frame + 81, 16);
                        memory_fill(frame + 81, 0, 16);
                        wifi_hmac_sha1(ptk, 16, frame, WIFI_EAPOL_HDR + data_length,
                                       check);
                        if (!crypto_same(mic, check, 16))
                        {
                                crypto_forget(check, sizeof(check));
                                continue;
                        }
                        crypto_forget(check, sizeof(check));
                }

                data = frame + 99;
                if (data_length >= WIFI_GTK_WRAP)
                {
                        p8 unwrapped[WIFI_WRAP_MOST];
                        positive plain = 0;
                        positive at = 0;

                        if (!wifi_kw_unwrap(ptk + 16, data, data_length, unwrapped,
                                            address_of plain) &&
                            !(data_length >= 8 + WIFI_GTK_WRAP &&
                              wifi_kw_unwrap(ptk + 16, data + 8, data_length - 8,
                                             unwrapped, address_of plain)))
                                plain = 0;

                        while (at + 2 <= plain)
                        {
                                p8 tag = unwrapped[at];
                                p8 room = unwrapped[at + 1];

                                if (at + 2 + room > plain)
                                        break;
                                if (tag == 0xdd && room >= 8 &&
                                    unwrapped[at + 2] == 0 &&
                                    unwrapped[at + 3] == 0x0f &&
                                    unwrapped[at + 4] == 0xac &&
                                    unwrapped[at + 5] == 1)
                                {
                                        gtk_idx = (p8)(unwrapped[at + 6] & 3);
                                        gtk_length = room - 6;
                                        if (gtk_length > sizeof(gtk))
                                                gtk_length = sizeof(gtk);
                                        memory_copy(gtk, unwrapped + at + 8, gtk_length);
                                        break;
                                }
                                at += 2 + room;
                        }
                        crypto_forget(unwrapped, sizeof(unwrapped));
                }

                memory_copy(replay, frame + 9, 8);
                memory_copy(rsc, frame + 65, 8);
                memory_fill(frame, 0, WIFI_EAPOL_HDR);
                frame[0] = 1;
                frame[1] = 3;
                network_store_16(frame + 2, 95);
                frame[4] = 2;
                network_store_16(frame + 5, 0x030a);
                network_store_16(frame + 7, 16);
                memory_copy(frame + 9, replay, 8);
                wifi_eapol_mic(ptk, frame, WIFI_EAPOL_HDR);
                if (wifi_eapol_send(eapol, index, bssid, frame, WIFI_EAPOL_HDR) < 0)
                        break;
                got = nl80211_new_key(session, index, 0, NL80211_KEYTYPE_PAIRWISE,
                                      bssid, ptk + 32, 16, null, 0, false);
                if (!got && gtk_length >= 16 && gtk_idx)
                        got = nl80211_new_key(session, index, gtk_idx,
                                              NL80211_KEYTYPE_GROUP, null, gtk, 16,
                                              rsc, 6, true);
                if (!got)
                        nl80211_authorize(session, index, bssid);
                crypto_forget(snonce, sizeof(snonce));
                crypto_forget(anonce, sizeof(anonce));
                crypto_forget(ptk, sizeof(ptk));
                crypto_forget(gtk, sizeof(gtk));
                crypto_forget(rsc, sizeof(rsc));
                crypto_forget(frame, sizeof(frame));
                return got;
        }

        crypto_forget(snonce, sizeof(snonce));
        crypto_forget(anonce, sizeof(anonce));
        crypto_forget(ptk, sizeof(ptk));
        crypto_forget(gtk, sizeof(gtk));
        crypto_forget(rsc, sizeof(rsc));
        crypto_forget(frame, sizeof(frame));
        return -110;
}

static COLD bool wifi_link_mac(string_address name, p8 address_to mac)
{
        netlink_search search;
        bipolar handle = netlink_open_groups(0);

        memory_fill(address_of search, 0, sizeof(search));
        if (handle < 0)
                return false;
        search.wanted = name;
        if (netlink_link_find((b32)handle, address_of search) < 0 ||
            !search.has_hardware)
        {
                socket_close((b32)handle);
                return false;
        }
        memory_copy(mac, search.hardware, 6);
        socket_close((b32)handle);
        return true;
}

static COLD bipolar nl80211_wait_associated(nl80211 address_to session, p32 sequence,
                                       p32 index, p8 address_to bssid)
{
        netlink_buffer reply = {0};
        network_deadline deadline;
        bool got_ack = false;
        bool associated = false;
        bipolar ack = 0;
        p64 last_poll = 0;

        if (!network_deadline_begin(address_of deadline, NL80211_CONNECT_SECONDS, 0))
                return -1;

        for (;;)
        {
                p32 local_port = 0;
                bipolar got;
                positive at = 0;

                if (got_ack && !associated && !session->mlme)
                {
                        p64 now = clock_monotonic_nanoseconds();

                        if (!last_poll || now - last_poll >= 200000000)
                        {
                                last_poll = now;
                                if (nl80211_station(session, index, bssid))
                                {
                                        netlink_forget(address_of reply);
                                        return 0;
                                }
                        }
                }

                got = network_wait_readable_until(session->handle, address_of deadline);
                if (got <= 0)
                {
                        netlink_forget(address_of reply);
                        if (got_ack && !associated &&
                            nl80211_station(session, index, bssid))
                                return 0;
                        return got < 0 ? got : -110;
                }

                got = netlink_receive(session->handle, address_of reply,
                                      address_of local_port);
                if (got == NETWORK_INTERRUPTED)
                        continue;
                if (got < 0)
                {
                        netlink_forget(address_of reply);
                        return got;
                }

                while (at + NETLINK_HEADER <= reply.used)
                {
                        netlink_header address_to header =
                            (netlink_header address_to)(reply.bytes + at);
                        p8 address_to body;

                        if (header->length < NETLINK_HEADER ||
                            at + header->length > reply.used)
                                break;
                        if (header->type == NLMSG_IS_ERROR &&
                            header->sequence == sequence &&
                            header->port == local_port)
                        {
                                ack = netlink_status(header, false);
                                got_ack = true;
                                if (ack < 0)
                                {
                                        netlink_forget(address_of reply);
                                        return ack;
                                }
                        }
                        else if (header->length >= NETLINK_HEADER + GENL_HEADER &&
                                 header->type == session->family)
                        {
                                body = (p8 address_to)header + NETLINK_HEADER;
                                if (nl80211_find_u32(header, NL80211_ATTR_IFINDEX, 0) ==
                                    index)
                                {
                                        if (body[0] == NL80211_CMD_CONNECT)
                                        {
                                                p16 status = nl80211_find_u16(
                                                    header, NL80211_ATTR_STATUS_CODE,
                                                    0);
                                                positive mac_length = 0;
                                                p8 address_to mac;

                                                if (nl80211_attr(header,
                                                                 NL80211_ATTR_TIMED_OUT))
                                                {
                                                        netlink_forget(address_of reply);
                                                        return -110;
                                                }
                                                if (status)
                                                {
                                                        netlink_forget(address_of reply);
                                                        return -111;
                                                }
                                                mac = (p8 address_to)netlink_find(
                                                    header, GENL_HEADER, NL80211_ATTR_MAC,
                                                    address_of mac_length);
                                                if (mac && mac_length >= 6)
                                                        memory_copy(bssid, mac, 6);
                                                associated = true;
                                        }
                                        else if (body[0] == NL80211_CMD_DISCONNECT &&
                                                 got_ack)
                                        {
                                                netlink_forget(address_of reply);
                                                return -111;
                                        }
                                }
                        }
                        at += netlink_align(header->length);
                }

                if (got_ack && associated)
                {
                        netlink_forget(address_of reply);
                        return 0;
                }
        }
}

static COLD bipolar nl80211_connect(nl80211 address_to session, p32 index,
                               p8 address_to ssid, positive ssid_length,
                               p8 address_to pmk, bool offload)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();
        p32 open = NL80211_AUTHTYPE_OPEN;
        p32 version = NL80211_WPA_VERSION_2;
        p32 ccmp = WLAN_CIPHER_CCMP;
        p32 psk = WLAN_AKM_PSK;
        bipolar sent;

        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_CONNECT, NLM_REQUEST | NLM_ACK, sequence))
                return -1;

        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        netlink_attribute_add(address_of request, NL80211_ATTR_SSID, ssid,
                              ssid_length);
        nl80211_attribute_u32(address_of request, NL80211_ATTR_AUTH_TYPE, open);

        if (pmk)
        {
                netlink_attribute_add(address_of request, NL80211_ATTR_PRIVACY, null,
                                      0);
                nl80211_attribute_u32(address_of request, NL80211_ATTR_WPA_VERSIONS,
                                      version);
                nl80211_attribute_u32(address_of request,
                                      NL80211_ATTR_CIPHER_SUITES_PAIRWISE, ccmp);
                nl80211_attribute_u32(address_of request,
                                      NL80211_ATTR_CIPHER_SUITE_GROUP, ccmp);
                nl80211_attribute_u32(address_of request, NL80211_ATTR_AKM_SUITES,
                                      psk);
                if (offload)
                        netlink_attribute_add(address_of request, NL80211_ATTR_PMK, pmk,
                                              32);
                else
                        netlink_attribute_add(address_of request,
                                              NL80211_ATTR_CONTROL_PORT, null, 0);
        }

        if (request.failed)
        {
                netlink_forget(address_of request);
                return -1;
        }

        sent = socket_send(session->handle, request.bytes, request.used, 0, 0, 0);
        netlink_forget(address_of request);
        if (sent < 0)
                return sent;
        return sequence;
}

static COLD bool nl80211_associated(void)
{
        nl80211 session;
        nl80211_iface iface;
        p8 mac[6];
        bool up = false;

        if (nl80211_open(address_of session) < 0)
                return false;
        if (!nl80211_interface(address_of session, address_of iface))
                up = nl80211_station(address_of session, iface.index, mac);
        nl80211_close(address_of session);
        return up;
}

static COLD bipolar nl80211_join(p8 address_to ssid, positive ssid_length, p8 address_to pmk)
{
        nl80211 session;
        nl80211_iface iface;
        bipolar route;
        bipolar failed;
        bipolar eapol = -1;
        p8 bssid[6];
        p8 sta[6];
        bool offload = false;
        bipolar sequence;

        memory_fill(bssid, 0, 6);
        memory_fill(sta, 0, 6);
        failed = nl80211_open(address_of session);
        if (failed < 0)
                return failed;

        failed = nl80211_interface(address_of session, address_of iface);
        if (failed < 0)
        {
                nl80211_close(address_of session);
                return failed;
        }

        route = netlink_open_groups(0);
        if (route >= 0)
        {
                netlink_link_up((b32)route, iface.index);
                socket_close((b32)route);
        }

        if (iface.has_mac)
                memory_copy(sta, iface.mac, 6);
        else
                wifi_link_mac((string_address)iface.name, sta);

        if (pmk)
                offload = nl80211_psk_offload(address_of session, iface.wiphy);

        if (pmk && !offload)
        {
                eapol = nl80211_eapol_open(iface.index);
                if (eapol < 0)
                {
                        nl80211_close(address_of session);
                        return eapol;
                }
        }

        nl80211_disconnect(address_of session, iface.index);

        sequence = nl80211_connect(address_of session, iface.index, ssid,
                                   ssid_length, pmk, offload);
        if (sequence < 0)
        {
                if (eapol >= 0)
                        socket_close((b32)eapol);
                nl80211_close(address_of session);
                return sequence;
        }

        failed = nl80211_wait_associated(address_of session, (p32)sequence, iface.index,
                                         bssid);
        if (!failed && pmk && !offload)
        {
                if (!(bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] |
                      bssid[5]))
                        nl80211_station(address_of session, iface.index, bssid);
                if (!(sta[0] | sta[1] | sta[2] | sta[3] | sta[4] | sta[5]) ||
                    !(bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] |
                      bssid[5]))
                        failed = -1;
                else
                        failed = wifi_handshake(address_of session, iface.index,
                                                (b32)eapol, sta, bssid, pmk);
        }
        if (failed && pmk && offload)
        {
                nl80211_disconnect(address_of session, iface.index);
                eapol = nl80211_eapol_open(iface.index);
                if (eapol >= 0)
                {
                        memory_fill(bssid, 0, 6);
                        sequence = nl80211_connect(address_of session, iface.index,
                                                   ssid, ssid_length, pmk, false);
                        if (sequence >= 0)
                        {
                                failed = nl80211_wait_associated(
                                    address_of session, (p32)sequence, iface.index,
                                    bssid);
                                if (!failed)
                                {
                                        if (!(bssid[0] | bssid[1] | bssid[2] |
                                              bssid[3] | bssid[4] | bssid[5]))
                                                nl80211_station(
                                                    address_of session,
                                                    iface.index, bssid);
                                        failed = wifi_handshake(
                                            address_of session, iface.index,
                                            (b32)eapol, sta, bssid, pmk);
                                }
                        }
                }
        }

        if (failed)
                nl80211_disconnect(address_of session, iface.index);

        if (eapol >= 0)
                socket_close((b32)eapol);
        nl80211_close(address_of session);
        return failed;
}

static bipolar nl80211_disconnect(nl80211 address_to session, p32 index)
{
        netlink_buffer request = {0};
        p32 sequence = netlink_sequence_take();

        if (!nl80211_begin(address_of request, session->family,
                           NL80211_CMD_DISCONNECT, NLM_REQUEST | NLM_ACK,
                           sequence))
                return -1;

        nl80211_attribute_u32(address_of request, NL80211_ATTR_IFINDEX, index);
        return netlink_transact(session->handle, address_of request, sequence,
                                null, null);
}

#endif


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
        /* text holds the saved passphrases in the clear, exactly as the
           radio_network array below does, so it is scrubbed the same way
           before this frame is left to whatever runs in it next. */

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

        crypto_forget(text, sizeof(text));
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
                {
                        crypto_forget(text, sizeof(text));
                        return -1;
                }
                memory_copy(text + used, networks[at].ssid,
                            networks[at].ssid_length);
                used += networks[at].ssid_length;
                text[used++] = '\n';
                memory_copy(text + used, networks[at].pass,
                            networks[at].pass_length);
                used += networks[at].pass_length;
                text[used++] = '\n';
        }

        {
                bipolar failed = host_write_file(NET_WIFI_LIST, text, used,
                                                 0600, true);

                crypto_forget(text, sizeof(text));
                return failed;
        }
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

/*      ----------------------------------------------------------------
        locale: the timezone and the clock, and the SNTP that sets it.
        ---------------------------------------------------------------- */

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

/*      ----------------------------------------------------------------
        SNTP: how the clock above is asked what the time is.
        ---------------------------------------------------------------- */

/*
        Experimental C standard library

        SNTP: RFC 5905 on-wire offset and delay, then a clock filter

        A single sample's offset error is (d_fwd - d_rev) / 2, and that is
        bounded by half the round-trip delay. Integer-second arithmetic and
        planting the server's transmit fraction as tv_nsec both throw that
        bound away. Five samples with the smallest delay kept is the RFC
        5905 clock filter, not an average: averaging a one-sided queue spike
        poisons the estimate.

        The clock is stepped by that offset with adjtimex ADJ_SETOFFSET, not
        by reading the clock again and planting a new wall time: the gap
        between those two traps would be extra error. A machine whose clock
        is still at the epoch must be allowed a decades-long first step; one
        whose clock already looks like a civil date may not jump more than a
        day, and a refresh of a synchronised clock may not jump more than
        two seconds. A kiss-o-death, a stratum 0, or a root delay/dispersion
        worse than a second abandons that server rather than collecting more
        samples from it.

        SNTP_WALL_MOST ends that window at 2036-01-01, which is before the
        NTP era rolls over rather than because of it: sntp_load_stamp reads
        the era from the top of the seconds field and converts either one.
        Moving the window is therefore the single edit that date needs, and
        it is safe to move. The offset sum is two differences of four terms
        the window bounds, and carrying the window all the way to 2104, the
        end of era 1, leaves that sum at 8.47 of the 9.22 the type holds.

        T1 is read, the originate stamp is written, and the packet is sent
        with nothing else in between. T4 is read the moment recv returns.
        The conversion of those timespecs into nanoseconds waits until after
        the trap. This tree has no vDSO: clock_gettime is a syscall, and
        that cost dwarfs the stores, so the C must not add a second one.

        WHAT HAS BEEN MEASURED, AND WHAT HAS NOT

        Against five servers at once, with the box's own clock held
        synchronised by something else as the reference, this file's
        answers sat within 193 microseconds of it; a single-sample client
        with userspace stamps, asked the same servers in the same minute,
        spread to 1207. Both agree on sign and scale, so the difference
        is the five-sample filter and the kernel stamps, not a disagreement
        about what time it is.

        The two kernel stamps are worth, at the median of real exchanges:
        10355 ns for the arrival stamp, and 1729 ns to one server and 2320
        to another for the departure stamp. Both are one-sided, which is
        why they land in the offset at all -- the formula assumes the path
        is symmetric. Round-trip asymmetry itself cannot be measured from
        one end and so cannot be corrected here; across the servers above
        it accounts for a spread of several milliseconds, which is larger
        than everything this file does about anything else.

        What is not proven is the clock being set. Nothing here can take
        CLOCK_REALTIME on a machine that is not ours to disturb, so the
        step and slew paths in locale.c have never been executed against a
        kernel that carried them out. What is checked is the request: the
        mode words against uapi/linux/timex.h, and the decision between
        stepping and slewing against crafted offsets in the machine lane.
        A reader should take "the right thing is asked for" from this and
        not "the asking has been seen to work".

        Dawn Larsson - Apache 2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_NET_SNTP
#define STANDARD_MODERN_C_NET_SNTP

#define SNTP_PORT 123
#define SNTP_PACKET 48
#define SNTP_SECONDS 2
#define SNTP_SAMPLES 5
#define SNTP_UNIX 2208988800u
#define SNTP_LI_VN_MODE 0x23
#define SNTP_NANOSECONDS 1000000000ull
#define SNTP_OK 0
#define SNTP_NO_SERVER (-1)
#define SNTP_NO_REPLY (-2)
#define SNTP_MALFORMED (-3)
#define SNTP_BAD_SERVER (-4)
#define SNTP_RATE_LIMITED (-5)
#define SNTP_KISS_RATE 0x52415445u /* "RATE" */
#define SNTP_KISS_DENY 0x44454e59u /* "DENY" */
#define SNTP_KISS_RSTR 0x52535452u /* "RSTR" */
#define SNTP_DELAY_MOST_NS ((bipolar)2 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_OFFSET_MOST_NS ((bipolar)24 * 3600 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_OFFSET_SYNCED_NS ((bipolar)2 * (bipolar)SNTP_NANOSECONDS)
#define SNTP_WALL_LEAST 1577836800ll /* 2020-01-01 */
#define SNTP_WALL_MOST 2082758400ll  /* 2036-01-01 */
#define SNTP_WALL_LEAST_NS \
        ((bipolar)SNTP_WALL_LEAST * (bipolar)SNTP_NANOSECONDS)
#define SNTP_WALL_MOST_NS \
        ((bipolar)SNTP_WALL_MOST * (bipolar)SNTP_NANOSECONDS)
#define SNTP_TIMESPEC_SECONDS_MOST 9223372035ull
#define SNTP_ERA ((bipolar)4294967296)
#define SNTP_SHORT_SECOND 0x10000u
#define SNTP_RANDOM_NONBLOCK 1
#define SNTP_TIMESTAMPNS 35
#define SNTP_TIMESTAMPING 37
#define SNTP_TIMESTAMPING_WANT 2194u /* TX_SOFTWARE|SOFTWARE|OPT_ID|TSONLY */
#define SNTP_ERRQUEUE 0x2000
#define SNTP_DONTWAIT 0x40
#define SNTP_MESSAGE_WORDS 7
#define SNTP_CONTROL_WORDS 16
#define SNTP_CONTROL_HEAD (sizeof(positive) + 8)
#define SNTP_ERRQUEUE_MOST 4
#define SNTP_SOL_IP 0
#define SNTP_IP_RECVERR 11
#define SNTP_ERROR_TIMESTAMPING 4 /* SO_EE_ORIGIN_TIMESTAMPING */
#define SNTP_ERROR_BYTES 16       /* struct sock_extended_err */
#define SNTP_ERROR_ORIGIN 4       /* ee_origin within it */
#define SNTP_ERROR_SEQUENCE 12    /* ee_data, which carries the id */
#define SNTP_CONTROL_DATA                          \
        ((SNTP_CONTROL_HEAD + sizeof(positive) - 1) & \
         ~(sizeof(positive) - 1))
#define SNTP_TEST_NOW \
        ((bipolar)1700000000 * (bipolar)SNTP_NANOSECONDS)

typedef struct
{
        bipolar offset_ns;
        bipolar delay_ns;
        bool ok;
} sntp_sample;

static inline INLINE CONST bool sntp_wall_ok(bipolar ns)
{
        return ns >= SNTP_WALL_LEAST_NS && ns <= SNTP_WALL_MOST_NS;
}

/*
        A local stamp may sit anywhere from the epoch to the end of the
        window, because a machine that has never been told the time boots
        at zero. It may not sit past the window: t1 and t4 are the only
        two terms sntp_offset_delay adds that are not already bounded by
        the wire format, and an unbounded one overflows the sum.
*/
static inline INLINE CONST bool sntp_local_ok(bipolar ns)
{
        return ns >= 0 && ns <= SNTP_WALL_MOST_NS;
}

static inline INLINE CONST bipolar sntp_timespec_ns(p64 seconds, p64 nanoseconds)
{
        if (nanoseconds >= SNTP_NANOSECONDS)
                return -1;
        if (seconds > SNTP_TIMESPEC_SECONDS_MOST)
                return -1;
        return (bipolar)seconds * (bipolar)SNTP_NANOSECONDS +
               (bipolar)nanoseconds;
}

static inline INLINE bipolar sntp_now_ns(void)
{
        p64 now[2] = {0, 0};

        if (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                          (positive)now) < 0)
                return -1;
        return sntp_timespec_ns(now[0], now[1]);
}

/*
        The transmit stamp goes out to be echoed back, and the echo is the
        only thing telling us a reply is ours. Sending the clock puts a
        number an off-path attacker can estimate into the one field it has
        to guess, and its low half is the whole of the guess: the seconds
        it already knows.

        Nothing reads this field back. t1 is taken from the timespec the
        trap filled, never from the packet, so the fraction carries no
        accuracy and a random one costs none. Thirty-two bits of it, on
        top of the source port the connected socket already randomises,
        is what the forgery has to match. The clock's own fraction is the
        fallback if the pool has no bytes to give, which is where this
        started.
*/
static inline INLINE fn sntp_put_stamp(p8 address_to field, p64 unix_seconds,
                                       p64 unix_nsec)
{
        p32 ntp_seconds = (p32)(unix_seconds + SNTP_UNIX);
        p32 ntp_frac =
            (p32)(((p64)unix_nsec << 32) / SNTP_NANOSECONDS);

        if (system_random_fill(address_of ntp_frac, sizeof(ntp_frac),
                               SNTP_RANDOM_NONBLOCK) < 0)
                ntp_frac = (p32)(((p64)unix_nsec << 32) / SNTP_NANOSECONDS);

        network_store_32(field, ntp_seconds);
        network_store_32(field + 4, ntp_frac);
}

static inline INLINE PURE bipolar sntp_load_stamp(p8 address_to field)
{
        p32 ntp_seconds = network_load_32(field);
        p32 ntp_frac = network_load_32(field + 4);
        bipolar unix_seconds = (bipolar)ntp_seconds - (bipolar)SNTP_UNIX;
        bipolar unix_nsec =
            (bipolar)(((p64)ntp_frac * SNTP_NANOSECONDS) >> 32);

        if (!(ntp_seconds & 0x80000000u))
                unix_seconds += SNTP_ERA;
        return unix_seconds * (bipolar)SNTP_NANOSECONDS + unix_nsec;
}

static inline INLINE CONST bool sntp_short_ok(p32 word)
{
        return !(word & 0x80000000u) && word <= SNTP_SHORT_SECOND;
}

static COLD fn sntp_split_offset(bipolar ns, bipolar address_to seconds,
                            bipolar address_to nanoseconds)
{
        bipolar sec = ns / (bipolar)SNTP_NANOSECONDS;
        bipolar nsec = ns % (bipolar)SNTP_NANOSECONDS;

        if (nsec < 0)
        {
                sec -= 1;
                nsec += (bipolar)SNTP_NANOSECONDS;
        }
        address_to seconds = sec;
        address_to nanoseconds = nsec;
}

static inline INLINE fn sntp_offset_delay(bipolar t1, bipolar t2, bipolar t3,
                                          bipolar t4,
                                          bipolar address_to offset_ns,
                                          bipolar address_to delay_ns)
{
        address_to offset_ns = ((t2 - t1) + (t3 - t4)) / 2;
        address_to delay_ns = (t4 - t1) - (t3 - t2);
}

static CONST COLD bool sntp_sample_sane(bipolar t1, bipolar t2, bipolar t3,
                                   bipolar t4, bipolar offset_ns,
                                   bipolar delay_ns, bool tight)
{
        if (t1 < 0 || t4 < t1 || t3 < t2)
                return false;
        if (!sntp_wall_ok(t2) || !sntp_wall_ok(t3))
                return false;
        if (delay_ns < 0 || delay_ns > SNTP_DELAY_MOST_NS)
                return false;
        if (tight)
        {
                if (offset_ns < -SNTP_OFFSET_SYNCED_NS ||
                    offset_ns > SNTP_OFFSET_SYNCED_NS)
                        return false;
        }
        else if (sntp_wall_ok(t1))
        {
                if (offset_ns < -SNTP_OFFSET_MOST_NS ||
                    offset_ns > SNTP_OFFSET_MOST_NS)
                        return false;
        }
        return true;
}

static COLD bool sntp_target_ok(bipolar now, bipolar offset_ns,
                           bipolar address_to target)
{
        if (offset_ns > 0 && now > bipolar_max - offset_ns)
                return false;
        if (offset_ns < 0 && now < bipolar_min - offset_ns)
                return false;
        address_to target = now + offset_ns;
        return sntp_wall_ok(address_to target);
}

static PURE COLD bipolar sntp_pick(sntp_sample address_to row, positive count)
{
        bipolar best = -1;
        positive at;

        for (at = 0; at < count; at++)
                if (row[at].ok &&
                    (best < 0 || row[at].delay_ns < row[best].delay_ns ||
                     (row[at].delay_ns == row[best].delay_ns &&
                      (bipolar)at > best)))
                        best = (bipolar)at;
        return best;
}

/*
        t4 is meant to be when the reply arrived. Read after recv returns
        it is when this process next ran, which is the arrival plus however
        long the packet waited in the socket and however long the scheduler
        took to wake us. Half of that lands in the offset, and on an idle
        machine talking to a real server it measured a shade over eleven
        microseconds -- four hundred times the whole of the arithmetic that
        follows, and nothing the arithmetic can do anything about.

        SO_TIMESTAMPNS makes the kernel record the arrival in the softirq
        that takes the packet off the device and hand it over as a control
        message. Reading it needs recvmsg rather than recvfrom, and the
        socket calls in library.c are recvfrom, so the trap is made here
        the way this file already traps for clock_gettime.

        msghdr is seven pointer-width words -- the name and its length,
        the vector and its count, the control buffer and its length, and
        the flags -- which is its shape on every target this tree builds.
        A control message is a pointer-width length, then a level and a
        type of four bytes each, then the payload at the next word.

        Nothing here is required to work. A kernel that refuses the option
        or a path that delivers no control message leaves the stamp unset,
        and the caller reads the clock itself exactly as before.
*/
/*
        The walk is apart from the trap because it is the half that fails
        quietly: a wrong offset finds no message, and a receive that found
        no message is indistinguishable from a kernel that sent none. That
        reads as "the timestamp did not help" rather than as a mistake, so
        it is reached here by its own name and sntp_math_ok hands it
        buffers laid out by hand -- including ones whose length words lie.

        Every field is read from a buffer the kernel filled, and the two
        lengths are believed only as far as the buffer goes: a message
        claiming to be longer than what is left ends the walk.
*/
static bool sntp_control_stamp(p8 address_to control, positive length,
                               b32 kind, p64 address_to arrived)
{
        positive at = 0;

        while (at + SNTP_CONTROL_DATA <= length)
        {
                positive size = address_to(positive address_to)(control + at);
                b32 level = address_to(b32 address_to)(control + at +
                                                       sizeof(positive));
                b32 type = address_to(b32 address_to)(control + at +
                                                      sizeof(positive) + 4);

                if (size < SNTP_CONTROL_DATA || size > length - at)
                        break;
                if (level == SOL_SOCKET && type == kind &&
                    size - SNTP_CONTROL_DATA >= 2 * sizeof(p64))
                {
                        arrived[0] = address_to(p64 address_to)(
                            control + at + SNTP_CONTROL_DATA);
                        arrived[1] = address_to(p64 address_to)(
                            control + at + SNTP_CONTROL_DATA + sizeof(p64));
                        return true;
                }
                at += (size + sizeof(positive) - 1) & ~(sizeof(positive) - 1);
        }
        return false;
}

static HOT bipolar sntp_receive_stamped(b32 handle, p8 address_to reply,
                                        positive room,
                                        p64 address_to arrived,
                                        bool address_to stamped)
{
        positive message[SNTP_MESSAGE_WORDS];
        positive vector[2];
        positive control[SNTP_CONTROL_WORDS];
        bipolar got;

        address_to stamped = false;
        memory_zero(message, sizeof(message));
        memory_zero(control, sizeof(control));
        vector[0] = (positive)reply;
        vector[1] = room;
        message[2] = (positive)vector;
        message[3] = 1;
        message[4] = (positive)control;
        message[5] = sizeof(control);

        got = system_call_3(syscall(recvmsg), (positive)handle,
                            (positive)message, SNTP_DONTWAIT);
        if_rare (got < 0)
                return got;

        address_to stamped = sntp_control_stamp((p8 address_to)control,
                                                message[5], SNTP_TIMESTAMPNS,
                                                arrived);
        return got;
}

/*
        t1 has the same trouble t4 had, at the other end. It is read
        before the send trap, so it is the moment before the kernel is
        entered, and the packet leaves after the protocol stack has run.
        The offset formula assumes the two directions are symmetric, so a
        head start on the send side alone goes straight into the answer at
        half its size: measured against the kernel's own departure stamp,
        a median of 1729 ns to one server and 2320 ns to another.

        SOF_TIMESTAMPING_TX_SOFTWARE records the moment the packet is
        given to the driver and queues it on the socket's error queue.
        Measured on a real route it is already there when send returns, 50
        times out of 50, so one recvmsg that refuses to wait collects it
        and no poll is needed.

        Draining it is not optional once the option is on. A socket with
        anything on its error queue reports POLLERR, and the wait below
        asks about readability and would be woken by that for ever. One
        non-blocking read after each send empties it, which 100 exchanges
        across two servers confirm: no POLLERR survived into the wait.

        OPT_TSONLY keeps the packet itself off the queue, so what comes
        back is the timestamp and the error header beside it. The walk
        steps over anything that is not the timestamp, which is what lets
        a real ICMP error sit there without being mistaken for one.
*/
/*
        OPT_ID is already asked for, so the kernel numbers every transmit
        stamp with a counter that starts at zero when the option is set
        and rises by one per send. Five sends on one socket came back 0,
        1, 2, 3, 4.

        The number does not travel in the timestamp. It is in the error
        header beside it, as ee_data, and the same header says in
        ee_origin whether this queue entry is a timestamp at all or a
        real ICMP error that happens to be sitting there. Reading both is
        what makes the stamp provably the one belonging to the send being
        timed, rather than whichever stamp was on the queue -- a
        distinction that only bites if a drain is ever missed, which is
        exactly the case that cannot be tested from outside.

        A kernel that sends no error header, or one whose numbering does
        not line up, leaves the stamp unclaimed and the exchange falls
        back to the userspace reading, as it does when the option is
        refused outright.
*/
static bool sntp_control_sequence(p8 address_to control, positive length,
                                  p32 address_to sequence)
{
        positive at = 0;

        while (at + SNTP_CONTROL_DATA <= length)
        {
                positive size = address_to(positive address_to)(control + at);
                b32 level = address_to(b32 address_to)(control + at +
                                                       sizeof(positive));
                b32 type = address_to(b32 address_to)(control + at +
                                                      sizeof(positive) + 4);

                if (size < SNTP_CONTROL_DATA || size > length - at)
                        break;
                if (level == SNTP_SOL_IP && type == SNTP_IP_RECVERR &&
                    size - SNTP_CONTROL_DATA >= SNTP_ERROR_BYTES)
                {
                        p8 address_to body = control + at + SNTP_CONTROL_DATA;

                        if (body[SNTP_ERROR_ORIGIN] == SNTP_ERROR_TIMESTAMPING)
                        {
                                address_to sequence =
                                    address_to(p32 address_to)(
                                        body + SNTP_ERROR_SEQUENCE);
                                return true;
                        }
                }
                at += (size + sizeof(positive) - 1) & ~(sizeof(positive) - 1);
        }
        return false;
}

static HOT bool sntp_transmit_stamp(b32 handle, p32 wanted,
                                    p64 address_to departed)
{
        positive message[SNTP_MESSAGE_WORDS];
        positive vector[2];
        positive control[SNTP_CONTROL_WORDS];
        p8 sink[SNTP_PACKET];
        p64 stamp[2];
        p32 sequence;
        bool found = false;
        positive round;

        for (round = 0; round < SNTP_ERRQUEUE_MOST; round++)
        {
                bipolar got;

                sequence = 0;
                memory_zero(message, sizeof(message));
                memory_zero(control, sizeof(control));
                vector[0] = (positive)sink;
                vector[1] = sizeof(sink);
                message[2] = (positive)vector;
                message[3] = 1;
                message[4] = (positive)control;
                message[5] = sizeof(control);
                got = system_call_3(syscall(recvmsg), (positive)handle,
                                    (positive)message,
                                    SNTP_ERRQUEUE | SNTP_DONTWAIT);
                if (got < 0)
                        break;
                if (sntp_control_stamp((p8 address_to)control, message[5],
                                       SNTP_TIMESTAMPING, stamp) &&
                    sntp_control_sequence((p8 address_to)control, message[5],
                                          address_of sequence) &&
                    sequence == wanted)
                {
                        departed[0] = stamp[0];
                        departed[1] = stamp[1];
                        found = true;
                }
        }
        return found;
}

/*
        Everything below is read out of forty-eight bytes a stranger sent.
        The socket is connected, so the kernel has already dropped a
        datagram whose source is not the server's, but an on-path answer
        and a blind one aimed at an open port both arrive here.

        The origin stamp is the check that carries the weight: it is the
        transmit stamp we planted, echoed back, and an answer that does
        not carry it was not an answer to our question. It is dropped and
        the wait resumes rather than ending the exchange, because a late
        reply to an earlier sample is not a reason to give up on this one.

        Stratum zero is a kiss-o-death and the four bytes at 12 say which.
        RATE is the server asking to be asked less often, which is a
        different answer from DENY and RSTR: it is reported separately so
        the policy above can wait instead of walking to the next server
        and asking again immediately.
*/
static COLD bipolar sntp_reply_ok(p8 address_to reply, p8 address_to request)
{
        if (memory_compare(reply + 24, request + 40, 8))
                return SNTP_NO_REPLY;
        if ((reply[0] & 0x7) != 4)
                return SNTP_BAD_SERVER;
        if (!reply[1])
                return network_load_32(reply + 12) == SNTP_KISS_RATE
                           ? SNTP_RATE_LIMITED
                           : SNTP_BAD_SERVER;
        if ((reply[0] >> 6) == 3 || reply[1] >= 16)
                return SNTP_BAD_SERVER;
        if (!sntp_short_ok(network_load_32(reply + 4)) ||
            !sntp_short_ok(network_load_32(reply + 8)))
                return SNTP_BAD_SERVER;
        return SNTP_OK;
}

static COLD bool sntp_math_ok(void)
{
        bipolar offset = 0;
        bipolar delay = 0;
        bipolar target = 0;
        bipolar two_days = (bipolar)2 * 86400 * (bipolar)SNTP_NANOSECONDS;
        positive at;
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        p8 control[96];
        p64 arrived[2];
        p32 sequence;
        static const struct
        {
                positive claimed; /* what the message says its length is */
                b32 level;
                b32 kind;
                positive held;    /* what the buffer actually holds */
                bool want;
        } control_case[] = {
            /* the message the kernel really sends */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, true},
            /* some other control message, of which there are many */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS + 1,
             SNTP_CONTROL_DATA + 16, false},
            {SNTP_CONTROL_DATA + 16, 0, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* a length word smaller than the header it heads */
            {SNTP_CONTROL_DATA - 8, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* a length word reaching past the end of the buffer */
            {SNTP_CONTROL_DATA + 64, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 16, false},
            /* the right message with too little room for a timespec */
            {SNTP_CONTROL_DATA + 8, SOL_SOCKET, SNTP_TIMESTAMPNS,
             SNTP_CONTROL_DATA + 8, false},
            /* nothing at all, which is what a kernel without the option
               sends, and the case the caller falls back on */
            {SNTP_CONTROL_DATA + 16, SOL_SOCKET, SNTP_TIMESTAMPNS, 0, false},
        };
        static const struct
        {
                p8 first;
                p8 stratum;
                p32 root_delay;
                p32 root_dispersion;
                p32 reference_id;
                bool echoed;
                bipolar want;
        } reply_case[] = {
            /* a stratum 2 server answering the question we asked */
            {0x24, 2, 0, 0, 0, true, SNTP_OK},
            /* the same packet with the origin stamp not echoed: a forgery,
               and the one check standing between us and an off-path lie */
            {0x24, 2, 0, 0, 0, false, SNTP_NO_REPLY},
            /* mode 3 is a request, not a reply */
            {0x23, 2, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* stratum 0 carries a kiss code in the reference id */
            {0x24, 0, 0, 0, SNTP_KISS_RATE, true, SNTP_RATE_LIMITED},
            {0x24, 0, 0, 0, SNTP_KISS_DENY, true, SNTP_BAD_SERVER},
            {0x24, 0, 0, 0, SNTP_KISS_RSTR, true, SNTP_BAD_SERVER},
            {0x24, 0, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* RATE is still RATE when the alarm bit is set with it */
            {0xe4, 0, 0, 0, SNTP_KISS_RATE, true, SNTP_RATE_LIMITED},
            /* stratum 16 is unsynchronised, and the alarm says so too */
            {0x24, 16, 0, 0, 0, true, SNTP_BAD_SERVER},
            {0xe4, 2, 0, 0, 0, true, SNTP_BAD_SERVER},
            /* a second of root delay is the most we trust, and the sign
               bit of the fixed-point short is never legitimately set */
            {0x24, 2, SNTP_SHORT_SECOND, SNTP_SHORT_SECOND, 0, true, SNTP_OK},
            {0x24, 2, SNTP_SHORT_SECOND + 1, 0, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0, SNTP_SHORT_SECOND + 1, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0x80000000u, 0, 0, true, SNTP_BAD_SERVER},
            {0x24, 2, 0, 0x80000000u, 0, true, SNTP_BAD_SERVER},
        };
        sntp_sample row[5] = {
            {10000000, 20000000, true}, {8000000, 80000000, true},
            {2000000, 15000000, true},  {4000000, 40000000, true},
            {50000000, 200000000, true},
        };
        static const bipolar delay_case[][6] = {
            {0, 1000000000, 1000000000, 2000000000, 0, 2000000000},
            {0, 1050000000, 1050000000, 2000000000, 50000000, 2000000000},
            {0, 100000000, 100000000, 1100000000, -450000000, 1100000000},
        };
        static const struct
        {
                p32 seconds;
                p32 fraction;
                bipolar want;
        } stamp_case[] = {
            /* era 0, the high bit set: 1968 through February 2036 */
            {SNTP_UNIX, 0, 0},
            {SNTP_UNIX + 1, 0, (bipolar)SNTP_NANOSECONDS},
            {SNTP_UNIX, 0x80000000u, 500000000},
            /* era 1, the high bit clear: February 2036 onward */
            {0, 0, (bipolar)2085978496 * (bipolar)SNTP_NANOSECONDS},
            {1, 0, (bipolar)2085978497 * (bipolar)SNTP_NANOSECONDS},
        };
        static const bipolar split_case[][3] = {
            {1500000000, 1, 500000000},
            {-1500000000, -2, 500000000},
            {-1, -1, 999999999},
            {0, 0, 0},
        };
        static const struct
        {
                bipolar t1;
                bipolar t2;
                bipolar t3;
                bipolar t4;
                bipolar off;
                bipolar del;
                bool tight;
                bool want;
        } sane_case[] = {
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 2000000000, 0,
             2000000000, false, true},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 2000000000, 0,
             2000000000, true, true},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 3000000001, 0,
             3000000001, false, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 1000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 500000000, 0,
             -500000000, false, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 3000000000,
             SNTP_TEST_NOW + 3000000000, SNTP_TEST_NOW + 50000000, 2975000000,
             50000000, true, false},
            {SNTP_TEST_NOW, SNTP_TEST_NOW + 2000000000,
             SNTP_TEST_NOW + 1000000000, SNTP_TEST_NOW + 50000000, 0, 50000000,
             false, false},
        };

        for (at = 0; at < array_count(delay_case); at++)
        {
                sntp_offset_delay(delay_case[at][0], delay_case[at][1],
                                  delay_case[at][2], delay_case[at][3],
                                  address_of offset, address_of delay);
                if (offset != delay_case[at][4] || delay != delay_case[at][5])
                        return false;
        }

        for (at = 0; at < array_count(sane_case); at++)
                if (sntp_sample_sane(sane_case[at].t1, sane_case[at].t2,
                                     sane_case[at].t3, sane_case[at].t4,
                                     sane_case[at].off, sane_case[at].del,
                                     sane_case[at].tight) != sane_case[at].want)
                        return false;

        sntp_offset_delay(0, SNTP_TEST_NOW + 1000000000,
                          SNTP_TEST_NOW + 1000000000, 100000000,
                          address_of offset, address_of delay);
        if (!sntp_sample_sane(0, SNTP_TEST_NOW + 1000000000,
                              SNTP_TEST_NOW + 1000000000, 100000000, offset,
                              delay, false) ||
            sntp_sample_sane(0, SNTP_TEST_NOW + 1000000000,
                             SNTP_TEST_NOW + 1000000000, 100000000, offset,
                             delay, true) ||
            !sntp_target_ok(0, offset, address_of target) ||
            target < SNTP_TEST_NOW || target > SNTP_TEST_NOW + 1000000000)
                return false;

        sntp_offset_delay(SNTP_TEST_NOW, SNTP_TEST_NOW + two_days,
                          SNTP_TEST_NOW + two_days,
                          SNTP_TEST_NOW + 50000000, address_of offset,
                          address_of delay);
        if (sntp_sample_sane(SNTP_TEST_NOW, SNTP_TEST_NOW + two_days,
                             SNTP_TEST_NOW + two_days,
                             SNTP_TEST_NOW + 50000000, offset, delay, false))
                return false;

        if (!sntp_short_ok(0) || !sntp_short_ok(SNTP_SHORT_SECOND) ||
            sntp_short_ok(SNTP_SHORT_SECOND + 1) ||
            sntp_short_ok(0x80000000u))
                return false;

        /*
                The seconds bound is the one that has to hold exactly: a
                whole second short of it, with the largest fraction, is
                still a number, and one second past it is not.
        */
        if (sntp_timespec_ns(SNTP_TIMESPEC_SECONDS_MOST,
                             SNTP_NANOSECONDS - 1) < 0 ||
            sntp_timespec_ns(SNTP_TIMESPEC_SECONDS_MOST + 1, 0) >= 0 ||
            sntp_timespec_ns(0, SNTP_NANOSECONDS) >= 0)
                return false;

        if (!sntp_local_ok(0) || !sntp_local_ok(SNTP_WALL_MOST_NS) ||
            sntp_local_ok(SNTP_WALL_MOST_NS + 1) || sntp_local_ok(-1))
                return false;

        for (at = 0; at < array_count(stamp_case); at++)
        {
                p8 field[8];

                network_store_32(field, stamp_case[at].seconds);
                network_store_32(field + 4, stamp_case[at].fraction);
                if (sntp_load_stamp(field) != stamp_case[at].want)
                        return false;
        }

        for (at = 0; at < array_count(split_case); at++)
        {
                sntp_split_offset(split_case[at][0], address_of offset,
                                  address_of delay);
                if (offset != split_case[at][1] || delay != split_case[at][2])
                        return false;
        }

        memory_fill(request, 0, sizeof(request));
        request[0] = SNTP_LI_VN_MODE;
        network_store_32(request + 40, 0xc0ffee00u);
        network_store_32(request + 44, 0x0badf00du);
        for (at = 0; at < array_count(reply_case); at++)
        {
                memory_fill(reply, 0, sizeof(reply));
                reply[0] = reply_case[at].first;
                reply[1] = reply_case[at].stratum;
                network_store_32(reply + 4, reply_case[at].root_delay);
                network_store_32(reply + 8, reply_case[at].root_dispersion);
                network_store_32(reply + 12, reply_case[at].reference_id);
                if (reply_case[at].echoed)
                        memory_copy(reply + 24, request + 40, 8);
                if (sntp_reply_ok(reply, request) != reply_case[at].want)
                        return false;
        }

        for (at = 0; at < array_count(control_case); at++)
        {
                memory_zero(control, sizeof(control));
                address_to(positive address_to)control = control_case[at].claimed;
                address_to(b32 address_to)(control + sizeof(positive)) =
                    control_case[at].level;
                address_to(b32 address_to)(control + sizeof(positive) + 4) =
                    control_case[at].kind;
                address_to(p64 address_to)(control + SNTP_CONTROL_DATA) =
                    1700000000ull;
                address_to(p64 address_to)(control + SNTP_CONTROL_DATA +
                                           sizeof(p64)) = 250000000ull;
                arrived[0] = 0;
                arrived[1] = 0;
                if (sntp_control_stamp(control, control_case[at].held,
                                       SNTP_TIMESTAMPNS,
                                       arrived) != control_case[at].want)
                        return false;
                if (control_case[at].want &&
                    (arrived[0] != 1700000000ull || arrived[1] != 250000000ull))
                        return false;
        }

        /*
                The kernel puts its messages in the order it likes, so the
                one we want is not always first. A message of another kind
                in front of it must be stepped over, not stopped at.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPNS + 7;
        address_to(positive address_to)(control + SNTP_CONTROL_DATA) =
            SNTP_CONTROL_DATA + 16;
        address_to(b32 address_to)(control + SNTP_CONTROL_DATA +
                                   sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + SNTP_CONTROL_DATA +
                                   sizeof(positive) + 4) = SNTP_TIMESTAMPNS;
        address_to(p64 address_to)(control + 2 * SNTP_CONTROL_DATA) =
            1700000001ull;
        address_to(p64 address_to)(control + 2 * SNTP_CONTROL_DATA +
                                   sizeof(p64)) = 750000000ull;
        arrived[0] = 0;
        arrived[1] = 0;
        if (!sntp_control_stamp(control, 2 * SNTP_CONTROL_DATA + 16,
                                SNTP_TIMESTAMPNS, arrived) ||
            arrived[0] != 1700000001ull || arrived[1] != 750000000ull)
                return false;

        /*
                The departure stamp comes back under a different type and
                in a longer payload -- three timespecs, of which the
                software one is first -- so the walk has to take its two
                words from the front and let the rest alone, and has to
                tell the two types apart rather than taking whichever
                timestamp it meets first.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 48;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPING;
        address_to(p64 address_to)(control + SNTP_CONTROL_DATA) = 1700000002ull;
        address_to(p64 address_to)(control + SNTP_CONTROL_DATA + sizeof(p64)) =
            125000000ull;
        arrived[0] = 0;
        arrived[1] = 0;
        if (!sntp_control_stamp(control, SNTP_CONTROL_DATA + 48,
                                SNTP_TIMESTAMPING, arrived) ||
            arrived[0] != 1700000002ull || arrived[1] != 125000000ull)
                return false;
        if (sntp_control_stamp(control, SNTP_CONTROL_DATA + 48,
                               SNTP_TIMESTAMPNS, arrived))
                return false;

        /*
                The transmit stamp's number rides in the error header
                beside it, not in the stamp, and the same header says
                whether the entry is a timestamp at all. A real ICMP
                error carries a different origin and must not be read as
                a sequence number, or a refused port would start
                claiming to be the answer to a send.
        */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA +
                                                 SNTP_ERROR_BYTES;
        address_to(b32 address_to)(control + sizeof(positive)) = SNTP_SOL_IP;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_IP_RECVERR;
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] =
            SNTP_ERROR_TIMESTAMPING;
        address_to(p32 address_to)(control + SNTP_CONTROL_DATA +
                                   SNTP_ERROR_SEQUENCE) = 4u;
        sequence = 0;
        if (!sntp_control_sequence(control, SNTP_CONTROL_DATA +
                                                SNTP_ERROR_BYTES,
                                   address_of sequence) ||
            sequence != 4u)
                return false;

        /* the same entry as an ICMP error rather than a timestamp */
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] = 2; /* ICMP */
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA +
                                               SNTP_ERROR_BYTES,
                                  address_of sequence))
                return false;

        /* a header cut short of the field the number sits in */
        control[SNTP_CONTROL_DATA + SNTP_ERROR_ORIGIN] =
            SNTP_ERROR_TIMESTAMPING;
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 8;
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA + 8,
                                  address_of sequence))
                return false;

        /* and no error header at all, which is the fallback case */
        memory_zero(control, sizeof(control));
        address_to(positive address_to)control = SNTP_CONTROL_DATA + 48;
        address_to(b32 address_to)(control + sizeof(positive)) = SOL_SOCKET;
        address_to(b32 address_to)(control + sizeof(positive) + 4) =
            SNTP_TIMESTAMPING;
        sequence = 0;
        if (sntp_control_sequence(control, SNTP_CONTROL_DATA + 48,
                                  address_of sequence))
                return false;

        /*
                One bit of the echoed stamp flipped is still a forgery.
        */
        memory_fill(reply, 0, sizeof(reply));
        reply[0] = 0x24;
        reply[1] = 2;
        memory_copy(reply + 24, request + 40, 8);
        reply[31] ^= 1;
        if (sntp_reply_ok(reply, request) != SNTP_NO_REPLY)
                return false;

        return sntp_pick(row, 5) == 2 && row[2].offset_ns == 2000000;
}

static HOT bipolar sntp_exchange(b32 handle,
                                 network_deadline address_to deadline,
                                 bool tight, p32 address_to sequence,
                                 sntp_sample address_to into)
{
        p8 request[SNTP_PACKET];
        p8 reply[SNTP_PACKET];
        p64 sent[2];
        p64 got[2];
        p64 spare[2];
        p32 mine;
        bipolar t1;
        bipolar t2;
        bipolar t3;
        bipolar t4;
        bipolar wait;
        bipolar received;
        bipolar verdict;
        bipolar reference;
        bool stamped;
        bipolar offset = 0;
        bipolar delay = 0;

        into->ok = false;
        memory_fill(request, 0, sizeof(request));
        request[0] = SNTP_LI_VN_MODE;

        if_rare (system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                               (positive)sent) < 0)
                return SNTP_NO_REPLY;
        sntp_put_stamp(request + 40, sent[0], sent[1]);
        if_rare (socket_send(handle, request, SNTP_PACKET, 0, 0, 0) < 0)
                return SNTP_NO_REPLY;
        mine = address_to sequence;
        address_to sequence = mine + 1;
        (void)sntp_transmit_stamp(handle, mine, sent);
        t1 = sntp_timespec_ns(sent[0], sent[1]);
        if_rare (!sntp_local_ok(t1))
                return SNTP_NO_REPLY;

        for (;;)
        {
                wait = network_wait_readable_until(handle, deadline);
                if_rare (wait <= 0)
                        return SNTP_NO_REPLY;
                received = sntp_receive_stamped(handle, reply,
                                                sizeof(reply), got,
                                                address_of stamped);
                if_rare (!stamped &&
                         system_call_2(syscall(clock_gettime), CLOCK_REALTIME,
                                       (positive)got) < 0)
                        return SNTP_NO_REPLY;
                if_rare (received < 0)
                {
                        /*
                                Nothing was readable, so what woke the
                                wait was the error queue: a transmit
                                stamp that was not yet there when the
                                send drained for it. Take it off now --
                                t1 is already decided, so the stamp is
                                of no further use -- because a socket
                                with anything on that queue reports
                                POLLERR, and leaving it would wake this
                                wait again immediately, and again, until
                                the deadline ran out.
                        */
                        (void)sntp_transmit_stamp(handle, mine, spare);
                        continue;
                }
                if_rare (received < SNTP_PACKET)
                        continue;
                verdict = sntp_reply_ok(reply, request);
                if_rare (verdict == SNTP_NO_REPLY)
                        continue;
                if_rare (verdict < 0)
                        return verdict;
                t4 = sntp_timespec_ns(got[0], got[1]);
                if_rare (!sntp_local_ok(t4))
                        return SNTP_MALFORMED;
                t2 = sntp_load_stamp(reply + 32);
                t3 = sntp_load_stamp(reply + 40);
                /*
                        The reference stamp is when the server last set
                        its own clock, so it sits at or before the stamp
                        it transmits. A second of slack, because the two
                        are read at different moments and a server whose
                        reference is one tick the wrong side of transmit
                        would otherwise be refused for ever: the sample
                        loop stops on BAD_SERVER, so that server is not
                        asked again.
                */
                reference = sntp_load_stamp(reply + 16);
                if_rare (!sntp_wall_ok(reference) ||
                         reference > t3 + (bipolar)SNTP_NANOSECONDS)
                        return SNTP_BAD_SERVER;
                sntp_offset_delay(t1, t2, t3, t4, address_of offset,
                                  address_of delay);
                if_rare (!sntp_sample_sane(t1, t2, t3, t4, offset, delay,
                                           tight))
                        return SNTP_MALFORMED;
                into->offset_ns = offset;
                into->delay_ns = delay;
                into->ok = true;
                return SNTP_OK;
        }
}

static COLD bipolar sntp_query_at(p32 server, bool filter, bool tight,
                             bipolar address_to offset_ns)
{
        socket_address_internet where = {
            .family = AF_INET,
            .port = network_order_16(SNTP_PORT),
            .host = network_order_32(server),
        };
        network_deadline deadline;
        sntp_sample row[SNTP_SAMPLES];
        positive want = filter ? SNTP_SAMPLES : 1;
        b32 want_stamp = 1;
        p32 want_transmit = SNTP_TIMESTAMPING_WANT;
        p32 sequence = 0;
        positive at;
        bipolar handle;
        bipolar best;
        bipolar failed = SNTP_NO_REPLY;

        if (!sntp_math_ok())
                return SNTP_MALFORMED;

        if (!network_deadline_begin(address_of deadline,
                                    SNTP_SECONDS * (filter ? SNTP_SAMPLES : 1),
                                    0))
                return SNTP_NO_REPLY;

        handle = socket_new(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return SNTP_NO_SERVER;
        if (socket_connect((b32)handle, address_of where, sizeof(where)) < 0)
        {
                socket_close((b32)handle);
                return SNTP_NO_SERVER;
        }
        (void)socket_option_set((b32)handle, SOL_SOCKET, SNTP_TIMESTAMPNS,
                                address_of want_stamp, sizeof(want_stamp));
        (void)socket_option_set((b32)handle, SOL_SOCKET, SNTP_TIMESTAMPING,
                                address_of want_transmit,
                                sizeof(want_transmit));

        memory_zero(row, sizeof(row));
        for (at = 0; at < want; at++)
        {
                failed = sntp_exchange((b32)handle, address_of deadline, tight,
                                       address_of sequence, row + at);
                if (failed == SNTP_BAD_SERVER || failed == SNTP_RATE_LIMITED)
                        break;
        }
        socket_close((b32)handle);

        best = sntp_pick(row, want);
        if (best < 0)
                return failed < 0 ? failed : SNTP_NO_REPLY;
        address_to offset_ns = row[best].offset_ns;
        return SNTP_OK;
}

static COLD bipolar sntp_query(string_address name, bool filter, bool tight,
                          bipolar address_to offset_ns)
{
        bipolar numeric;
        p32 host = 0;
        bipolar found;

        if (!name || !name[0])
                return SNTP_NO_SERVER;
        numeric = string_to_host(name);
        if (numeric >= 0)
                return sntp_query_at((p32)numeric, filter, tight, offset_ns);

        found = dns_resolve_any((string_address) "/etc/resolv.conf", name,
                                address_of host, SNTP_SECONDS);
        if (found != DNS_OK)
                return SNTP_NO_SERVER;
        return sntp_query_at(host, filter, tight, offset_ns);
}

#endif


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
        guard below fed a call that could not set the clock, said it
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


/*
        Forget userspace, keep the machine.

        /home is emptied. /root is emptied except the overlay and the files
        an image update already leaves on the data partition. /bowls
        stays: that is the pre-installed software a kiosk starts after wipe.
        The builtin machine script calls this at every settled boot.
*/
static string_address host_wipe_keep[] = {
    "main.moonwater.sh",
    "wifi",
    "wifi.power",
    "bluetooth",
    "bluetooth.power",
    "internet",
    "timezone",
    "ntp",
    "ntp.server",
    "ntp.filter",
    "keyboard",
    null,
};

static bipolar host_wipe_ensure(string_address path, positive mode)
{
        bipolar made = system_make_directory_at(AT_FDCWD, path, mode);

        return made < 0 && made != -ERROR_EXISTS ? made : 0;
}

static b32 host_wipe(void)
{
        bipolar failed;

        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater wipe");

        failed = bowl_reset_walk("/home", 0);
        if (failed < 0)
                return host_fail("/home", failed);

        failed = host_wipe_ensure("/home", 0755);
        if (failed < 0)
                return host_fail("/home", failed);

        failed = bowl_reset_walk_at(AT_FDCWD, "/root", 0, true, host_wipe_keep);
        if (failed < 0)
                return host_fail("/root", failed);

        failed = host_wipe_ensure("/root", 0700);
        if (failed < 0)
                return host_fail("/root", failed);

        string_format(log, host_label "userspace forgotten\n");
        log_flush();
        return 0;
}

/*
        Bound events, and the init and exit lists, as one verb.

        A kernel event is one line. init and exit stay lists, because more
        than one thing runs at boot and at stop. An empty line puts the
        event's default back. What is not the default is kept in the image
        and put back at the next boot.
*/
static fn host_bind_say(string_address prefix, struct bind_control address_to control)
{
        p16 line = host_machine_event_line(control->event);

        if (line)
                string_format(log, "%s%s: %s:%p\n", prefix,
                              (string_address)control->name, host_machine_where(),
                              (positive)line);
        else if (!control->command[0])
                string_format(log, "%s%s\n", prefix, (string_address)control->name);
        else
                string_format(log, "%s%s: %s\n", prefix, (string_address)control->name,
                              (string_address)control->command);
}

static fn host_bind_forget(host_settings address_to settings, p16 event)
{
        host_setting setting;
        positive at;
        bool dropped;

        for (;;)
        {
                at = 0;
                dropped = false;
                while (host_settings_next(settings, address_of at, address_of setting))
                {
                        if (setting.entry.list == SPARK_SETTINGS_BIND &&
                            setting.entry.id == event)
                        {
                                host_settings_drop(settings, address_of setting);
                                dropped = true;
                                break;
                        }
                }
                if (!dropped)
                        return;
        }
}

static b32 host_bind_keep(unsigned int event, struct bind_control address_to control)
{
        host_settings settings;
        p16 id = (p16)event;
        string_address failed;

        host_state_ready();
        host_settings_session(address_of settings);
        host_bind_forget(address_of settings, id);
        if (!(control->flags & SPARK_BIND_DEFAULT))
        {
                failed = host_settings_add(address_of settings, SPARK_SETTINGS_BIND,
                                           SPARK_SETTINGS_COMMAND,
                                           (string_address)control->command,
                                           string_length((string_address)control->command),
                                           address_of id);
                if (failed)
                        return host_settings_refused("bind", failed, address_of settings);
        }
        host_settings_save(address_of settings);
        return 0;
}

static fn host_bind_apply(host_settings address_to settings)
{
        host_setting setting;
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        struct bind_control control;
        positive at = 0;
        bipolar device;

        device = system_open_at(AT_FDCWD, SPARK_DEVICE, FILE_READ | O_CLOEXEC);
        if (device < 0)
                return;

        while (host_settings_next(settings, address_of at, address_of setting))
        {
                if (setting.entry.list != SPARK_SETTINGS_BIND)
                        continue;

                host_settings_text(text, address_of setting);
                host_bind_ioctl(device, SPARK_BIND_SET, setting.entry.id, text,
                                address_of control);
        }

        system_close(device);
}

static bipolar host_bind_each(string_address prefix, bool required)
{
        struct bind_control control;
        bipolar device;
        bipolar failed;
        unsigned int event;
        unsigned int count = SPARK_BIND_EVENTS;

        device = system_open_at(AT_FDCWD, SPARK_DEVICE, FILE_READ | O_CLOEXEC);
        if (device < 0)
                return required ? device : 0;

        for (event = 1; event <= count; event++)
        {
                failed = host_bind_ioctl(device, SPARK_BIND_GET, event, null,
                                         address_of control);
                if (failed < 0)
                {
                        system_close(device);
                        return (required && event == 1) ? failed : 0;
                }
                if (event == 1 && control.count)
                        count = control.count;
                host_bind_say(prefix, address_of control);
        }

        system_close(device);
        return 0;
}

static b32 host_bind_events(void)
{
        bipolar failed = host_bind_each("  ", true);

        if (failed < 0)
                return host_fail(SPARK_DEVICE, failed);

        string_format(log, "  reset is the keyboard's reset/restart key; "
                           "a case reset button cannot be bound\n");
        log_flush();
        return 0;
}

static fn host_bind_names(writer out)
{
        unsigned int event;
        bool first = true;

        for (event = 0; event < SPARK_BIND_EVENTS; event++)
        {
                string_format(out, "%s%s", first ? "" : ", ",
                              (string_address)spark_bind_event_name[event]);
                first = false;
        }
}

static unsigned int host_bind_named(string_address first, string_address second)
{
        p8 wanted[SPARK_BIND_NAME_MAX];
        unsigned int event;

        wanted[0] = end;
        string_append_bounded(wanted, first, sizeof(wanted));
        if (second)
        {
                string_append_bounded(wanted, " ", sizeof(wanted));
                string_append_bounded(wanted, second, sizeof(wanted));
        }

        for (event = 0; event < SPARK_BIND_EVENTS; event++)
                if (string_equals((string_address)spark_bind_event_name[event], wanted))
                        return event + 1;

        return 0;
}

static b32 host_bind_tell(unsigned int event, string_address command)
{
        struct bind_control control;
        p16 line = host_machine_event_line(event);
        bipolar failed;

        if (line)
        {
                host_machine_refused(event && event <= SPARK_BIND_EVENTS
                                         ? (string_address)spark_bind_event_name[event - 1]
                                         : (string_address) "that event",
                                     line);
                return 1;
        }

        failed = host_bind_request(SPARK_BIND_SET, event, command,
                                   address_of control);

        if (failed == -EPERM)
                return host_refuse(control.flags & SPARK_BIND_BOOT
                                           ? "setting what %s runs needs root (CAP_SYS_ADMIN and CAP_SYS_BOOT)\n"
                                           : "setting what %s runs needs root (CAP_SYS_ADMIN)\n",
                                   control.name[0] ? (string_address)control.name
                                                   : (string_address)"that event");
        if (failed == -ENAMETOOLONG)
                return host_refuse("that command is longer than the %s a bound event holds\n",
                                   "255 bytes");
        if (failed < 0)
                return host_fail(SPARK_DEVICE, failed);

        if (host_bind_keep(event, address_of control))
                return 1;
        host_bind_say(host_label, address_of control);
        log_flush();
        return 0;
}

/* moonwater bind [init|exit|EVENT ...] */
static b32 host_bind(string_address address_to arguments, positive count)
{
        struct bind_control probe;
        p8 text[SPARK_BIND_COMMAND_MAX];
        unsigned int event;
        string_address second = null;
        positive words;
        positive length = 0;

        if (count == 2)
                return host_bind_events();

        if (string_equals(arguments[2], "init") || string_equals(arguments[2], "exit"))
        {
                arguments[1] = arguments[2];
                for (positive at = 3; at < count; at++)
                        arguments[at - 1] = arguments[at];
                return host_settings_command(arguments, count - 1);
        }

        event = 0;
        words = 3;
        if (count >= 4)
        {
                event = host_bind_named(arguments[2], arguments[3]);
                if (event)
                {
                        second = arguments[3];
                        words = 4;
                }
        }
        if (!event)
                event = host_bind_named(arguments[2], null);

        if (!event)
        {
                if (host_bind_request(SPARK_BIND_GET, 1, null, address_of probe) < 0)
                        return host_fail(SPARK_DEVICE, -ENODEV);

                if (second)
                        string_format(log_error,
                                      host_label "%s %s is not a bound event; the events are ",
                                      arguments[2], arguments[3]);
                else
                        string_format(log_error, host_label "%s is not a bound event; the events are ",
                                      arguments[2]);
                host_bind_names(log_error);
                string_format(log_error, "\n");
                log_flush();
                return 1;
        }

        if (count == words)
        {
                struct bind_control control;
                bipolar failed = host_bind_request(SPARK_BIND_GET, event, null,
                                                   address_of control);

                if (failed < 0)
                        return host_fail(SPARK_DEVICE, failed);
                host_bind_say(host_label, address_of control);
                log_flush();
                return 0;
        }

        if (!host_settings_words(text, sizeof(text), arguments + words, count - words,
                                 address_of length))
                return host_refuse("that command is longer than the %s a bound event holds\n",
                                   "255 bytes");

        return host_bind_tell(event, text);
}

// The command ---------------------------------------------------

static fn host_title(writer out)
{
        string_format(out, TERM_BOLD "Moonwater" TERM_RESET "\n\n");
}

/*
        One list as it is typed, then what it does. Status, help and a
        wrong argument share it, so the picture and the usage read as the
        same page.
*/
static fn host_usage_write(writer out)
{
        string_format(out,
                      TERM_BOLD "  status" TERM_RESET
                      "                      " TERM_DIM "this picture" TERM_RESET "\n"
                      TERM_BOLD "  install DISK [--removable]" TERM_RESET
                      "  " TERM_DIM "put Moonwater on a disk" TERM_RESET "\n"
                      TERM_BOLD "  use [DISK]" TERM_RESET
                      "                  " TERM_DIM "keep that disk this session" TERM_RESET "\n"
                      TERM_BOLD "  update [DISK]" TERM_RESET
                      "               " TERM_DIM "write this build onto a disk" TERM_RESET "\n"
                      TERM_BOLD "  live" TERM_RESET
                      "                        " TERM_DIM "leave the disks alone" TERM_RESET "\n"
                      TERM_BOLD "  bind" TERM_RESET
                      "                        " TERM_DIM "what the machine's events run" TERM_RESET "\n"
                      TERM_BOLD "  bind EVENT [COMMAND]" TERM_RESET
                      "        " TERM_DIM "one event; empty puts the default back" TERM_RESET "\n"
                      TERM_BOLD "  bind init [add|remove ...]" TERM_RESET
                      "  " TERM_DIM "what runs at boot" TERM_RESET "\n"
                      TERM_BOLD "  bind init mount [on|off]" TERM_RESET
                      "    " TERM_DIM "mount kept disks at boot" TERM_RESET "\n"
                      TERM_BOLD "  bind exit [add|remove ...]" TERM_RESET
                      "  " TERM_DIM "what runs when the machine stops" TERM_RESET "\n"
                      TERM_BOLD "  canvas [on|off]" TERM_RESET
                      "             " TERM_DIM "the desktop" TERM_RESET "\n"
                      TERM_BOLD "  wifi [on|off]" TERM_RESET
                      "               " TERM_DIM "the wireless radio" TERM_RESET "\n"
                      TERM_BOLD "  wifi add SSID [PASSWORD]" TERM_RESET
                      "    " TERM_DIM "remember a network and join it" TERM_RESET "\n"
                      TERM_BOLD "  bluetooth [on|off]" TERM_RESET
                      "          " TERM_DIM "the bluetooth radio" TERM_RESET "\n"
                      TERM_BOLD "  bluetooth add NAME" TERM_RESET
                      "          " TERM_DIM "remember a bluetooth device" TERM_RESET "\n"
                      TERM_BOLD "  priority internet [wired|wifi]" TERM_RESET
                      " " TERM_DIM "which link when both are up [wired]" TERM_RESET "\n"
                      TERM_BOLD "  timezone [ZONE]" TERM_RESET
                      "             " TERM_DIM "the clock's zone [UTC]" TERM_RESET "\n"
                      TERM_BOLD "  ntp [on|off]" TERM_RESET
                      "                " TERM_DIM "set the clock from the network [on]" TERM_RESET "\n"
                      TERM_BOLD "  ntp filter [on|off]" TERM_RESET
                      "         " TERM_DIM "keep the lowest-delay sample of five [on]" TERM_RESET "\n"
                      TERM_BOLD "  keyboard [LAYOUT]" TERM_RESET
                      "           " TERM_DIM "Canvas keys: us uk de se no dk fi fr es it" TERM_RESET "\n"
                      TERM_BOLD "  wipe" TERM_RESET
                      "                        " TERM_DIM "forget /home and /root, keep the machine" TERM_RESET "\n"
                      "\n"
                      TERM_DIM                       "  Settings stay in the image this session started from.\n"
                      "  install takes this session's; update keeps the disk's.\n"
                      "  The machine script overwrites bind, init and exit.\n"
                      "  " HOST_MACHINE_SCRIPT " overlays the kernel builtin.\n" TERM_RESET);
        log_flush();
}

static b32 host_usage(void)
{
        host_title(log_error);
        host_usage_write(log_error);
        return 2;
}

/*
        This session as one page: the build, the disks, Canvas, the bound
        events, init and exit, then the commands. Nothing is a log line;
        the words that name each fact stay as they are.
*/
static b32 host_status(void)
{
        p8 running[HOST_BUILD_ROOM];
        p8 verdict[HOST_NAME_ROOM + 16];
        host_census census;
        host_medium_search search;
        host_settings settings;
        struct canvas_control canvas;

        host_state_ready();
        host_title(log);

        if (host_running_build(running, sizeof(running)))
                string_format(log, "  this is %s\n", running);
        else
                string_format(log, "  this build's version cannot be read\n");

        host_read_text(HOST_VERDICT, verdict, sizeof(verdict));
        if (host_starts(verdict, "disk "))
                string_format(log, "  kept on %s: %s /root /home\n",
                              verdict + 5, BOWL_ROOT_DIRECTORY);
        else if (host_starts(verdict, "ask "))
                string_format(log, "  waiting: %s has another build; "
                                   "moonwater use, update or live\n",
                              verdict + 4);
        else
                string_format(log, "  live: nothing is kept after power off\n");

        host_census_take(address_of census);
        for (positive at = 0; at < census.count; at++)
        {
                host_install address_to install = census.found + at;

                host_install_read(install);
                if (!install->readable)
                        string_format(log, "  installed on %s, image unreadable\n",
                                      install->disk);
                else if (string_equals(install->build, running))
                        string_format(log, "  installed on %s, this build\n",
                                      install->disk);
                else
                        string_format(log, "  installed on %s, another build: %s\n",
                                      install->disk, install->build);
        }

        if (!census.count)
                string_format(log, "  not installed on any disk here\n");

        if (running[0] && host_medium_find(address_of search, running, null))
        {
                string_format(log, "  this image is on %s\n", search.name);
                host_unmount(HOST_MEDIUM);
        }

        string_format(log, "\n");

        if (host_canvas_request(SPARK_CANVAS_STATUS, address_of canvas) >= 0)
                host_canvas_write("  ", address_of canvas);

        {
                p8 zone[80];
                p8 keyboard[16];

                locale_word(LOCALE_ZONE_PATH, zone, sizeof(zone));
                locale_word(LOCALE_KEYBOARD_PATH, keyboard, sizeof(keyboard));
                string_format(log, "  timezone %s\n",
                              zone[0] ? (string_address)zone
                                      : (string_address) "UTC");
                string_format(log, "  ntp %s, filter %s\n",
                              locale_ntp_wanted() ? (string_address) "on"
                                                  : (string_address) "off",
                              locale_ntp_filter_wanted()
                                  ? (string_address) "on"
                                  : (string_address) "off");
                string_format(log, "  keyboard %s\n",
                              keyboard[0] ? (string_address)keyboard
                                          : (string_address) "us");
        }

        (void)host_bind_each("  ", false);

        host_settings_session(address_of settings);
        host_settings_lines(address_of settings, 0, true);
        string_format(log, "  init mount is %s\n",
                      settings.flags & SPARK_SETTINGS_MOUNT_OFF ? "off" : "on");
        host_settings_lines(address_of settings, 1, true);

        string_format(log, "\n");
        host_usage_write(log);
        return 0;
}

/* use and update: the named install, the one boot asked about, or the only one. */
static b32 host_answer(bool update, string_address disk)
{
        p8 verdict[HOST_NAME_ROOM + 16];
        host_census census;
        host_install address_to install = null;

        host_read_text(HOST_VERDICT, verdict, sizeof(verdict));
        if (disk && host_starts(disk, "/dev/"))
                disk += 5;

        /*      The data this session already keeps. An update touches only the
                system partition, so it can go ahead underneath what is mounted;
                keeping a second disk's data on top of the first cannot. */
        if (host_starts(verdict, "disk "))
        {
                if (disk && !string_equals(disk, verdict + 5))
                        return host_refuse("this session already keeps %s\n",
                                           verdict + 5);

                if (update)
                {
                        host_census_take(address_of census);
                        install = host_census_find(address_of census, verdict + 5);
                        return install ? host_update(install)
                                       : host_refuse("%s is no longer here\n",
                                                     verdict + 5);
                }

                string_format(log, host_label "already kept on %s\n", verdict + 5);
                log_flush();
                return 0;
        }

        host_census_take(address_of census);

        if (disk)
                install = host_census_find(address_of census, disk);
        else if (host_starts(verdict, "ask "))
                install = host_census_find(address_of census, verdict + 4);
        else if (census.count == 1)
                install = census.found;

        if (!install)
                return host_refuse(census.count > 1 ? "more than one disk has "
                                                      "Moonwater; name one%s\n"
                                   : disk        ? "%s has no Moonwater install\n"
                                                 : "no disk here has Moonwater "
                                                   "installed%s\n",
                                   disk ? disk : (string_address)"");

        return host_take(install, update);
}

static b32 host_main()
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        string_address verb = count > 1 ? arguments[1] : null;

        if (!verb || string_equals(verb, "-h") || string_equals(verb, "--help"))
        {
                if (count > 2)
                        return host_usage();

                host_title(log);
                host_usage_write(log);
                return 0;
        }

        if (string_equals(verb, "status") && count <= 2)
                return host_status();

        // Before the root check: reading needs nothing, and the kernel
        // decides who may set a bound event. init and exit still need root.
        if (string_equals(verb, "bind"))
                return host_bind(arguments, count);

        if (string_equals(verb, "canvas"))
                return host_canvas(arguments, count);

        if (string_equals(verb, "wifi") || string_equals(verb, "bluetooth") ||
            string_equals(verb, "priority"))
                return host_radio(arguments, count);

        if (string_equals(verb, "timezone") || string_equals(verb, "ntp") ||
            string_equals(verb, "keyboard"))
                return host_locale(arguments, count);

        if (string_equals(verb, "wipe") && count == 2)
                return host_wipe();

        if (string_equals(verb, "machine") && count == 2)
                return host_machine_run();

        if (!string_equals(verb, "install") && !string_equals(verb, "use") &&
            !string_equals(verb, "update") && !string_equals(verb, "live") &&
            !string_equals(verb, "boot") && !string_equals(verb, "ask"))
                return host_usage();

        if (!bowl_is_root())
                return host_refuse("%s needs root\n", "moonwater");

        host_state_ready();

        if (string_equals(verb, "install"))
        {
                string_address disk = null;
                bool removable = false;

                for (positive at = 2; at < count; at++)
                {
                        if (string_equals(arguments[at], "--removable"))
                                removable = true;
                        else if (!disk)
                                disk = arguments[at];
                        else
                                return host_usage();
                }

                return disk ? host_install_disk(disk, removable) : host_usage();
        }

        if (count > 3 || (count > 2 && (string_equals(verb, "live") ||
                                        string_equals(verb, "boot") ||
                                        string_equals(verb, "ask"))))
                return host_usage();

        if (string_equals(verb, "boot"))
                return host_boot();

        if (string_equals(verb, "live"))
        {
                system_remove_at(AT_FDCWD, HOST_QUESTION, 0);
                host_verdict_set("live", "");
                string_format(log, host_label "the disks are left alone this session\n");
                log_flush();
                return 0;
        }

        if (string_equals(verb, "ask"))
        {
                p8 verdict[HOST_NAME_ROOM + 16];

                host_read_text(HOST_VERDICT, verdict, sizeof(verdict));
                if (!host_starts(verdict, "ask ") ||
                    system_rename_at(AT_FDCWD, HOST_QUESTION, AT_FDCWD,
                                     HOST_QUESTION_TAKEN, 0) < 0)
                        return host_refuse("there is nothing to ask%s\n", "");

                host_question(verdict + 4);
                return 0;
        }

        return host_answer(string_equals(verb, "update"),
                           count == 3 ? arguments[2] : null);
}

#define MOONWATER_CLI
#include "../moonwater.c"
