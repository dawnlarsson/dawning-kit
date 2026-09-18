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

static bipolar host_write_text(string_address path, string_address text)
{
        bipolar handle = system_open_at_mode(AT_FDCWD, path,
                                             FILE_WRITE | O_CLOEXEC, 0644);
        bipolar failed;

        if (handle < 0)
                return handle;

        failed = storage_format_write(handle, (p8 address_to)text,
                                      string_length(text), 0);
        system_close(handle);
        return failed;
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

        if ((!system && !data) || !host_parent(name, parent, sizeof(parent)))
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
        return true;
}

/* Every disk with both of Moonwater's partitions on it, by their names. */
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
        string_address who;
        positive who_length = 0;

        into[0] = end;
        if (!file_machine_read(address_of machine) ||
            host_read_text("/proc/version", banner, sizeof(banner)) <= 0)
                return false;

        release = string_length(machine.release);
        version = string_length(machine.version);
        if (memory_compare(banner, head, sizeof(head) - 1) ||
            memory_compare(banner + sizeof(head) - 1, machine.release, release) ||
            memory_compare(banner + sizeof(head) - 1 + release, " (", 2))
                return false;

        who = banner + sizeof(head) - 1 + release + 2;
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

static fn host_unmount(string_address target)
{
        system_call_2(syscall(umount2), (positive)target, 0);
}

static fn host_install_read(host_install address_to install)
{
        p8 path[HOST_PATH_ROOM];

        install->readable = false;
        install->build[0] = end;

        if (host_mount(install->system, HOST_LOOK, "vfat", HOST_READ_ONLY) < 0)
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
        bipolar failed = host_mount(install->data, HOST_DATA, "ext4", 0);
        positive at;

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

typedef struct
{
        string_address disk;
        host_install address_to install;
} host_partition_look;

static bool host_partition_visit(string_address directory, string_address name,
                                 address_any opaque)
{
        host_partition_look address_to look = (host_partition_look address_to)opaque;
        p8 path[HOST_PATH_ROOM];
        p8 number[16];

        if (!host_starts(name, look->disk) || string_length(name) >= HOST_NAME_ROOM ||
            !host_join(path, sizeof(path), directory, "/") ||
            !host_join(path, sizeof(path), path, name) ||
            !host_join(path, sizeof(path), path, "/partition") ||
            host_read_text(path, number, sizeof(number)) < 0)
                return true;

        if (string_equals(number, "1"))
                string_copy(look->install->system, name);
        else if (string_equals(number, "2"))
                string_copy(look->install->data, name);

        return true;
}

/* The kernel's names for the partitions just written, once their nodes exist. */
static bool host_partitions_wait(host_install address_to install)
{
        p8 sysfs[HOST_PATH_ROOM];
        p8 node[HOST_PATH_ROOM];
        host_partition_look look = {install->disk, install};

        if (!host_join(sysfs, sizeof(sysfs), "/sys/class/block/", install->disk))
                return false;

        for (positive tries = 0; tries < 50; tries++)
        {
                install->system[0] = install->data[0] = end;
                host_each_entry(sysfs, host_partition_visit, address_of look);

                if (install->system[0] && install->data[0] &&
                    host_join(node, sizeof(node), "/dev/", install->system) &&
                    system_access_at(AT_FDCWD, node, 0) >= 0 &&
                    host_join(node, sizeof(node), "/dev/", install->data) &&
                    system_access_at(AT_FDCWD, node, 0) >= 0)
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
        handle = system_open_at(AT_FDCWD, device, FILE_READ | O_CLOEXEC);
        failed = handle;
        if (handle >= 0)
        {
                failed = system_control(handle, HOST_BLKGETSIZE64, address_of bytes);
                if (failed >= 0)
                        failed = system_control(handle, HOST_BLKSSZGET,
                                                address_of sector);
                system_close(handle);
        }

        if (failed < 0 || bytes < HOST_SMALLEST || sector < 512 ||
            !storage_gpt_span(bytes / (p64)sector, (p32)sector, address_of first,
                              address_of last))
        {
                host_unmount(HOST_MEDIUM);
                return failed < 0 ? host_fail(device, failed)
                                  : host_refuse("%s is smaller than the 2 GiB an "
                                                "install needs\n", name);
        }

        if (!host_join(path, sizeof(path), sysfs, "/device/model") ||
            host_read_text(path, text, sizeof(text)) <= 0)
                string_copy(text, "a disk");

        string_format(log, host_label "Installing erases everything on %s: %s, %p GiB.\n"
                           host_label "Type %s to go on: ",
                      name, text, bytes >> 30, name);
        log_flush();

        if (host_read_line(answer, sizeof(answer)) < 0 ||
            !string_equals(answer, name))
        {
                host_unmount(HOST_MEDIUM);
                string_format(log, host_label "nothing written\n");
                log_flush();
                return 1;
        }

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

        if (failed || !host_partitions_wait(address_of target))
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

/* This session's settings: its own copy, else the one image of this build a disk here has, else the defaults. */
static fn host_settings_session(host_settings address_to settings)
{
        host_settings_search search;
        p8 running[HOST_BUILD_ROOM];

        if (host_settings_kept(settings) || host_settings_booted(settings))
                return;

        host_settings_empty(settings);
        memory_zero(address_of search, sizeof(search));

        if (!host_running_build(running, sizeof(running)))
                return;

        search.build = running;
        storage_each_device(host_settings_visit, address_of search);

        if (search.count == 1)
                memory_copy_apart(settings, address_of search.found, SPARK_SETTINGS_SLOT);
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

static fn host_settings_list(host_settings address_to settings, positive which)
{
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        host_setting setting;
        positive at = 0;
        positive shown = 0;
        p16 line = host_machine_hook_line(host_lists[which].hook);

        if (line)
        {
                string_format(log, host_label "%s is %s:%p\n",
                              host_lists[which].verb, host_machine_where(),
                              (positive)line);
                log_flush();
                return;
        }

        while (host_settings_next(settings, address_of at, address_of setting))
        {
                if (setting.entry.list != host_lists[which].list)
                        continue;

                host_settings_text(text, address_of setting);
                string_format(log, "%p  %s\n", (positive)setting.entry.id, text);
                shown++;
        }

        if (!shown)
                string_format(log, host_label "%s: %s\n", host_lists[which].verb,
                              host_lists[which].empty);

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
        positive which = 0;
        p16 id = 0;
        p8 list;

        while (which < array_count(host_lists) && !string_equals(verb, host_lists[which].verb))
                which++;

        if (which == array_count(host_lists))
                return HOST_SETTINGS_USAGE;

        list = host_lists[which].list;

        if (count == 2)
        {
                host_settings_list(settings, which);
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

        if (host_mount(install->system, HOST_LOOK, "vfat", HOST_READ_ONLY) < 0)
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

#include "radio.c"

/*
        Forget userspace, keep the machine.

        /home is emptied. /root is emptied except the overlay and the radio
        files an image update already leaves on the data partition. /bowls
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
};

static bool host_wipe_kept_name(string_address name)
{
        for (positive at = 0; at < array_count(host_wipe_keep); at++)
                if (string_equals(name, host_wipe_keep[at]))
                        return true;

        return false;
}

static bipolar host_wipe_ensure(string_address path, positive mode)
{
        bipolar made = system_make_directory_at(AT_FDCWD, path, mode);

        return made < 0 && made != -ERROR_EXISTS ? made : 0;
}

static bipolar host_wipe_root(void)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        bipolar failed = 0;

        if (!file_walk_open(address_of walk, AT_FDCWD, "/root"))
                return walk.error == -ERROR_NO_ENTRY ? 0 : walk.error;

        while (!failed && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name) || host_wipe_kept_name(entry->d_name))
                        continue;

                if (file_is_directory(walk.handle, entry->d_name))
                {
                        failed = bowl_reset_walk_at(walk.handle, entry->d_name, 0);
                        if (!failed)
                                failed = system_remove_at(walk.handle, entry->d_name,
                                                          AT_REMOVEDIR);
                }
                else
                        failed = system_remove_at(walk.handle, entry->d_name, 0);

                if (failed == -ERROR_NO_ENTRY)
                        failed = 0;
        }

        if (!failed)
                failed = walk.error;
        file_walk_close(address_of walk);
        return failed;
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

        failed = host_wipe_root();
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

static fn host_status_events(host_settings address_to settings, positive which)
{
        p8 text[SPARK_SETTINGS_TEXT_MOST + 1];
        host_setting setting;
        positive at = 0;
        positive shown = 0;
        p16 line = host_machine_hook_line(host_lists[which].hook);

        if (line)
        {
                string_format(log, "  %s: %s:%p\n", host_lists[which].verb,
                              host_machine_where(),
                              (positive)line);
                return;
        }

        while (host_settings_next(settings, address_of at, address_of setting))
        {
                if (setting.entry.list != host_lists[which].list)
                        continue;

                if (!shown)
                        string_format(log, "  %s:\n", host_lists[which].verb);

                host_settings_text(text, address_of setting);
                string_format(log, "    %p  %s\n", (positive)setting.entry.id, text);
                shown++;
        }

        if (!shown)
                string_format(log, "  %s: %s\n", host_lists[which].verb,
                              host_lists[which].empty);
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

        (void)host_bind_each("  ", false);

        host_settings_session(address_of settings);
        host_status_events(address_of settings, 0);
        string_format(log, "  init mount is %s\n",
                      settings.flags & SPARK_SETTINGS_MOUNT_OFF ? "off" : "on");
        host_status_events(address_of settings, 1);

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
