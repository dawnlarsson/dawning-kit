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
static b32 host_place_image(string_address system, string_address running)
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

        failed = host_place_image(HOST_SYSTEM, running);
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
        return 0;
}

// Boot ----------------------------------------------------------

static b32 host_boot(void)
{
        p8 running[HOST_BUILD_ROOM];
        host_census census;
        host_install address_to chosen = null;

        host_state_ready();
        host_running_build(running, sizeof(running));

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
                if (!host_take(chosen, false))
                        return 0;

                host_verdict_set("live", "");
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
                return host_refuse("%s is removable media; moonwater install %s "
                                   "--removable is the way to mean it\n", name);

        if (!host_running_build(running, sizeof(running)))
                return host_refuse("%s cannot read its own build\n", "moonwater");

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

        string_format(log, host_label "writing this build to %s, from %s\n",
                      target.system, search.name);
        log_flush();

        failed = host_place_image(HOST_SYSTEM, running);
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

// The command ---------------------------------------------------

static b32 host_status(void)
{
        p8 running[HOST_BUILD_ROOM];
        p8 verdict[HOST_NAME_ROOM + 16];
        host_census census;
        host_medium_search search;

        host_state_ready();

        if (host_running_build(running, sizeof(running)))
                string_format(log, host_label "this is %s\n", running);
        else
                string_format(log, host_label "this build's version cannot be read\n");

        host_read_text(HOST_VERDICT, verdict, sizeof(verdict));
        if (host_starts(verdict, "disk "))
                string_format(log, host_label "kept on %s: %s /root /home\n",
                              verdict + 5, BOWL_ROOT_DIRECTORY);
        else if (host_starts(verdict, "ask "))
                string_format(log, host_label "waiting: %s has another build; "
                                              "moonwater use, update or live\n",
                              verdict + 4);
        else
                string_format(log, host_label "live: nothing is kept after power off\n");

        host_census_take(address_of census);
        for (positive at = 0; at < census.count; at++)
        {
                host_install address_to install = census.found + at;

                host_install_read(install);
                if (!install->readable)
                        string_format(log, host_label "installed on %s, image unreadable\n",
                                      install->disk);
                else if (string_equals(install->build, running))
                        string_format(log, host_label "installed on %s, this build\n",
                                      install->disk);
                else
                        string_format(log, host_label "installed on %s, another build: %s\n",
                                      install->disk, install->build);
        }

        if (!census.count)
                string_format(log, host_label "not installed on any disk here\n");

        if (running[0] && host_medium_find(address_of search, running, null))
        {
                string_format(log, host_label "this image is on %s\n", search.name);
                host_unmount(HOST_MEDIUM);
        }

        log_flush();
        return 0;
}

static b32 host_usage(void)
{
        string_format(log_error,
                      host_label "usage: moonwater [status]\n"
                      host_label "       moonwater install DISK [--removable]\n"
                      host_label "       moonwater use [DISK]\n"
                      host_label "       moonwater update [DISK]\n"
                      host_label "       moonwater live\n");
        log_flush();
        return 2;
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
        string_address verb = count > 1 ? arguments[1] : (string_address)"status";

        if (string_equals(verb, "status") && count <= 2)
                return host_status();

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
