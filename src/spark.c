/*
        spark binary format

        A flat executable with no relocations, no dynamic linking and no
        section table -- just three regions the kernel maps directly.

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        File layout, every region a whole number of pages:

                offset          contents                  mapped as
                0               header, then text+rodata  read + execute
                text_size       data                      read + write
                --              bss                       read + write, zeroed

        The header shares the first page with the code rather than occupying a
        page of its own: it is mapped read only along with the text, entry
        simply points past it, and every image is 4096 bytes smaller.

        The image is not position independent: the code carries absolute
        addresses, so it has to land at the base recorded in the header. The
        producer and the loader agree on that base through this header rather
        than through a constant compiled into both.

        Keeping every region page aligned is what lets the loader map each one
        with a single vm_mmap and no partial page fixups: bss is a plain
        anonymous mapping, already zero, with nothing to clear by hand.
*/

#ifndef SPARK_INCLUDED
#define SPARK_INCLUDED

#include "platform/spark.inc"

// "SPRK", little endian
#define SPARK_MAGIC 0x4b525053u

#define SPARK_VERSION 1
#define SPARK_PAGE 4096

// Anything below this in the file cannot be a valid image. The kernel
// pre-reads BINPRM_BUF_SIZE (256) bytes for us, so the header must fit there.
#define SPARK_HEADER_SIZE 64

// An arbitrary ceiling, well past anything a flat binary should be, that keeps
// the loader's arithmetic on file supplied sizes far from overflowing.
#define SPARK_MAX_IMAGE (256UL << 20)

/*
        Kernel-to-runtime startup handoff.

        A Spark image does not need to interrogate hardware the kernel already
        brought up and classified.  The loader places this magic in one
        callee-saved entry register and the feature word in the next one.
        Ordinary ELF execution and stock kernels do not promise either value,
        so _start falls back to its own detector unless both sides speak this
        exact contract.
*/

struct header {
        unsigned int magic;    // SPARK_MAGIC
        unsigned short version;// SPARK_VERSION
        unsigned short flags;  // reserved, must be 0
        unsigned long base;    // virtual address the text region maps at
        unsigned long entry;   // first instruction, absolute
        unsigned long text_size; // page multiple, read + execute
        unsigned long data_size; // page multiple, read + write
        unsigned long bss_size;  // page multiple, read + write, zero filled
        unsigned long reserved[2]; // pads the header to exactly SPARK_HEADER_SIZE
};

// The loader reads the header out of the kernel's pre-read buffer and the
// producer writes it byte by byte, so the two only agree while this holds.
_Static_assert(sizeof(struct header) == SPARK_HEADER_SIZE,
               "spark header must be exactly SPARK_HEADER_SIZE bytes");


/*
        Spawning

        fork builds a complete copy of the caller -- address space, page
        tables, file table -- and exec then throws the address space half away
        microseconds later. For spawning a fresh program none of that copy is
        ever read. Measured on this kernel it costs about 3us of the ~10.5us a
        fork+exec spawn takes.

        A spawn that creates the task with no address space to copy skips both
        the duplication and the teardown. /dev/spark exposes that: write a
        request, get back a pid you can wait on exactly like a forked child.

        This is a device rather than a syscall on purpose. A syscall would mean
        patching arch/x86/entry/syscalls/syscall_64.tbl in the kernel tree,
        which this repo downloads rather than tracks, so it would become a
        patch to re-apply on every kernel bump.

        argv arrives as one flat block of NUL terminated strings so the whole
        request copies in with a single copy_from_user:

                "/bin/thing\0-v\0file\0"   argv_count = 3
*/

#define SPARK_DEVICE "/dev/spark"

// misc major, with a fixed minor from the range reserved for local use, so the
// node can be created statically in the initramfs without devtmpfs.
#define SPARK_DEVICE_MAJOR 10
#define SPARK_DEVICE_MINOR 250

// _IOW('s', 1, struct spawn) -- spelled out so userspace does not need
// the kernel ioctl macros to talk to it. The size is part of the encoding, so
// this changes if the request struct does. Generation-keyed environments make
// repeated launches on one open descriptor a pointer handoff after the first.
//
// One opcode carries every launch. What used to be five differed only in two
// independent booleans and how many descriptors trailed the request, so the
// kernel decoded a cross product of them from the opcode number and copied
// the descriptors in a second time at an offset that depended on which one
// arrived. Both now live in the request itself, where a caller can set them
// in any combination and the loader reads them in the one copy it already
// does.
#define SPARK_IOCTL_SPAWN 0x40407301u

// The shell's ENOEXEC rule: executable text without a #! line is handed to
// /bin/sh. Off by default so a raw spawn stays an exact execve-like interface
// and callers that do not want shell interpretation cannot acquire it by
// accident.
#define SPARK_SPAWN_SHELL 0x1u

// Take argv[0] as a utility in the immutable system /shell image rather than
// as a path to open.
#define SPARK_SPAWN_TOOL 0x2u

#define SPARK_SPAWN_FLAGS (SPARK_SPAWN_SHELL | SPARK_SPAWN_TOOL)

/* What SPARK_SPAWN_TOOL runs. Part of the flag's meaning rather than a path
   the kernel picks, so it is written here beside it and nowhere else. */
#define SPARK_TOOL_PROGRAM "/shell"

/* What the compositor starts once it has a screen. A root-level link to the
   shell image, which the build makes for every applet the SYSTEM category
   holds -- so the name here has to stay one of those, and the image_nodes
   harness is what says so. src/sh/tools.inc is the list. */
#define SPARK_TERMINAL_PROGRAM "/term"

// _IOR('s', 2, struct stats). Nanoseconds accumulated inside the kernel,
// so the split between creating the task and loading the image is measured
// where it happens rather than inferred from the outside.
#define SPARK_IOCTL_STATS 0x80307302u

struct stats {
        unsigned long spawns;
        unsigned long task_ns; // time inside user_mode_thread
        unsigned long exec_ns;   // time inside kernel_execve
        unsigned long loader_ns; // time inside the spark binfmt handler
        unsigned long loads;     // binfmt invocations, which exceed spawns:
                                 // a spark image run by ordinary exec loads
                                 // too, without going through the device
        unsigned long map_ns;    // of the handler, just the region mapping --
                                 // the part that is actually ours to optimise
};

// _IOR('s', 3, struct input_stats). Nanoseconds from a pointer event
// reaching the kernel to the cursor being on screen, and what the
// acceleration curve did with the counts a mouse reported.
#define SPARK_IOCTL_INPUT_STATS 0x80707303u

struct input_stats {
        unsigned long events;
        unsigned long mean_ns;
        unsigned long worst_ns;
        unsigned long queue_ns;
        unsigned long draw_ns;
        unsigned long flush_ns;
        unsigned long counts;     // reported by the device
        unsigned long moved;      // pixels the cursor was moved by them
        unsigned long composes;   // full passes over every output
        unsigned long compose_ns; // spent in them
        unsigned long painted;    // pixels written, all drawing
        unsigned long runs;       // calls into the row primitives
        unsigned long driver_ns;  // of compose_ns, the driver's share
        unsigned long text_ns;    // and the share spent laying out glyphs
};

// _IOR('s', 6, struct cursor_stats). Kept separate from input_stats so the
// existing diagnostic ABI and its encoded structure size remain stable.
#define SPARK_IOCTL_CURSOR_STATS 0x80407306u

struct cursor_stats {
        unsigned long requested_generation; // urgent drag/resize sync request
        unsigned long armed_generation;     // last all-plane completion
        unsigned long updates;               // successful visible plane arms
        unsigned long failures;              // runtime paint/update failures
        int requested_x, requested_y;
        int armed_x, armed_y;       // last all-plane completed request
        unsigned int active;        // outputs retaining a hardware plane
        unsigned int shown;         // active planes currently showing it
        unsigned int wanted;        // outputs containing the logical cursor
        unsigned int recovering;    // a full commit still has to clear a plane
};

// _IOR('s', 7, struct input_devices). Every input device the compositor is
// attached to, whether it opened, and how many reports it has delivered.
// A mouse that is dead until it is plugged in again is either one the
// kernel never heard from or one whose reports went nowhere, and only a
// count per device tells the two apart from a stuck cursor.
#define SPARK_IOCTL_INPUT_DEVICES 0x82487307u
#define INPUT_DEVICES_MAX 8

struct input_device_stats {
        char name[56];
        unsigned long events;  // reports delivered to the compositor
        long opened;           // 0 once open, else the error the last try gave
};

struct input_devices {
        unsigned long count;   // devices attached, listed or not
        struct input_device_stats device[INPUT_DEVICES_MAX];
};

/*
        One versioned view of the kernel data read by system utilities.

        The caller chooses sections, owns the output buffer and receives
        offsets rather than pointers.  That keeps the ABI relocatable and
        lets one ioctl replace a forest of open/read/parse/close cycles.  A
        stock kernel can produce the exact same records from procfs, which is
        important: acceleration must not change namespace or visibility
        semantics merely because /dev/spark exists.

        Counters use their native, lossless units.  CPU and process time is
        nanoseconds, memory and network values are bytes, and load is fixed at
        two decimal places.  Every snapshot also carries the three common
        clocks, regardless of its sections; zero sections requests just that
        common metadata.  Consumers decide presentation.
*/
#define SPARK_SNAPSHOT_VERSION 1u

#define SPARK_SNAPSHOT_SYSTEM  0x01u
#define SPARK_SNAPSHOT_CPU     0x02u
#define SPARK_SNAPSHOT_NETWORK 0x04u
#define SPARK_SNAPSHOT_PROCESS 0x08u
#define SPARK_SNAPSHOT_KERNEL  (SPARK_SNAPSHOT_SYSTEM | SPARK_SNAPSHOT_CPU | \
                                SPARK_SNAPSHOT_NETWORK)
#define SPARK_SNAPSHOT_ALL     (SPARK_SNAPSHOT_KERNEL | SPARK_SNAPSHOT_PROCESS)
#define SPARK_SNAPSHOT_MAX_BYTES (16u << 20)

struct snapshot_header {
        unsigned int version;
        unsigned int flags;
        unsigned int bytes;
        unsigned int page_size;
        unsigned long monotonic_ns;
        unsigned long realtime_seconds;
        unsigned long uptime_ns;
        unsigned long memory_total;
        unsigned long memory_available;
        unsigned long swap_total;
        unsigned long swap_free;
        unsigned int load[3];
        unsigned int cpu_offset;
        unsigned int cpu_count;
        unsigned int network_offset;
        unsigned int network_count;
        unsigned int process_offset;
        unsigned int process_count;
        unsigned int reserved;
};

struct snapshot_cpu {
        unsigned int id;       // ~0u is the all-CPU aggregate
        unsigned int reserved;
        unsigned long total_ns;
        unsigned long idle_ns;
};

struct snapshot_network {
        char name[16];
        unsigned long received;
        unsigned long transmitted;
};

struct snapshot_process {
        unsigned int pid;
        unsigned int ppid;
        unsigned int pgrp;
        unsigned int session;
        int tty;
        int tpgid;
        int nice;
        unsigned int threads;
        unsigned long user_ns;
        unsigned long system_ns;
        unsigned long start_ns;
        unsigned long virtual_bytes;
        unsigned long resident_bytes;
        unsigned int uid;
        unsigned int state;
        char command[16];
};

struct snapshot_request {
        unsigned long buffer;
        unsigned int capacity;
        unsigned int flags;
        unsigned int version;
        unsigned int used;
        unsigned int required;
        unsigned int reserved;
};

_Static_assert(sizeof(struct snapshot_header) == 112,
               "spark snapshot header ABI");
_Static_assert(sizeof(struct snapshot_cpu) == 24,
               "spark snapshot CPU ABI");
_Static_assert(sizeof(struct snapshot_network) == 32,
               "spark snapshot network ABI");
_Static_assert(sizeof(struct snapshot_process) == 96,
               "spark snapshot process ABI");
_Static_assert(sizeof(struct snapshot_request) == 32,
               "spark snapshot request ABI");

// _IOWR('s', 9, struct snapshot_request)
#define SPARK_IOCTL_SNAPSHOT 0xc0207309u

/*
        The request numbers, in one place, because more than one change adds
        them at once: 1 spawn, 2 stats, 3 input stats, 4 window create
        (window.c), 5 window commit (window.c), 6 cursor stats, 7 input
        devices, 9 snapshot, 10 Canvas on and off, 11 bindings, 12 and 13
        reading and setting the boot settings. 8 was never used and stays
        that way. 14 and 15 are Moonwater's machine process and script;
        they are defined in moonwater.c so Spark stays the image and spawn
        device. The next request takes the next free number past the
        highest, 16 at the time of writing, and a gap is never filled: an
        old program sending an old number must never reach a new request
        that happens to share it.
*/

/*
        Canvas, on and off.

        Off gives the display back: every program's window is asked to close,
        the compositor's input handler and thread stop, its DRM clients are
        released, every console gets its keyboard back, and the kernel's
        framebuffer console takes each screen. On takes the cards again and
        opens the kernel log and a terminal; it refuses while another program
        is master of a card, and names that program.

        The state below comes back whatever the request answers. Reading it
        needs nothing; on and off need CAP_SYS_ADMIN. LAYOUT reads the
        compositor keymap without a capability; setting it needs CAP_SYS_ADMIN.
*/
#define SPARK_CANVAS_STATUS 0u
#define SPARK_CANVAS_ON 1u
#define SPARK_CANVAS_OFF 2u
#define SPARK_CANVAS_LAYOUT 3u
#define SPARK_CANVAS_OUTPUTS 4u

struct canvas_output_state {
        char connector[16];
        unsigned int width, height, refresh;
        unsigned int reserved;
};

struct canvas_control {
        unsigned int request;      // SPARK_CANVAS_*
        unsigned int running;      // 1 while Canvas holds a card
        unsigned int cards;        // cards Canvas holds
        unsigned int windows;      // programs' windows on the desktop
        unsigned int detached;     // windows off closed that are still open
        unsigned int suspended;    // 1 while another program is a card's master
        int master_pid;            // who held a card on refused, 0 for nobody
        unsigned int output_count; // outputs below, at most SPARK_CANVAS_OUTPUTS
        char master_command[16];
        char driver[16];           // the first card's driver
        struct canvas_output_state output[SPARK_CANVAS_OUTPUTS];
};

_Static_assert(sizeof(struct canvas_output_state) == 32, "spark canvas output ABI");
_Static_assert(sizeof(struct canvas_control) == 192, "spark canvas control ABI");

// _IOWR('s', 10, struct canvas_control)
#define SPARK_IOCTL_CANVAS 0xc0c0730au

/*
        Bindings: what the machine's own events run.

        Every event has a name and an id, and an id is never given to another
        event, so an old program asking for one can never reach a new one.
        A bound event runs its line the way `/shell -c` does, as root with
        every capability -- unless a machine-script function or case arm owns
        that event, in which case it is queued into the attached process.
        `function moonwater_poweroff` is the hard binding; `moonwater_event`
        still runs for every event when that hook exists. The line lasts until
        the machine stops; keeping it
        across a boot belongs to whoever sets it at boot. An empty line puts
        back the event's default, which for most events is nothing.

        poweroff, reset and ctrl_alt_delete default to poweroff, reboot and
        reboot. canvas on and canvas off run when the desktop starts (the
        first card at boot, or `moonwater canvas on`) and when it stops,
        not instead of `moonwater canvas on|off`. init and exit are lists
        in the settings block, not rows here.

        Reading needs nothing. Setting needs CAP_SYS_ADMIN, because the line
        runs as root, and CAP_SYS_BOOT as well for the events flagged
        SPARK_BIND_BOOT, which stop, sleep or restart the machine: a process
        allowed only to stop the machine must not choose what runs.

        A machine-script function `moonwater_<event>`, or a literal case arm
        in `moonwater_event`, owns that event while the kernel holds the
        script: bind_fire queues into the attached process instead of
        spawning the image line, and `moonwater bind` prints the script line
        rather than SET. `moonwater_event` itself, when present, is queued
        every event; `*)` is not ownership. Events the overlay does not name
        still use the image binds. SET of the image line is still allowed (the
        fallback if the machine process is not attached); the CLI refuses to
        change an owned event so the two copies cannot drift from the keyboard.

        The 312-byte request is a different ioctl from the 272-byte power
        button that used this number; a stale caller gets ENOTTY.
*/
#define SPARK_BIND_COMMAND_MAX 256u
#define SPARK_BIND_NAME_MAX 24u

#define SPARK_BIND_GET 0u
#define SPARK_BIND_SET 1u

#define SPARK_BIND_DEFAULT 0x1u // the line is the event's default
#define SPARK_BIND_RUNNING 0x2u // a run is in flight
#define SPARK_BIND_PENDING 0x4u // a run is queued
#define SPARK_BIND_BOOT 0x8u    // setting it needs CAP_SYS_BOOT as well

#define SPARK_BIND_POWEROFF 1u
#define SPARK_BIND_SLEEP 2u
#define SPARK_BIND_RESET 3u
#define SPARK_BIND_CTRL_ALT_DELETE 4u
#define SPARK_BIND_LID_CLOSE 5u
#define SPARK_BIND_LID_OPEN 6u
#define SPARK_BIND_VOLUME_UP 7u
#define SPARK_BIND_VOLUME_DOWN 8u
#define SPARK_BIND_MUTE 9u
#define SPARK_BIND_BRIGHTNESS_UP 10u
#define SPARK_BIND_BRIGHTNESS_DOWN 11u
#define SPARK_BIND_CANVAS_ON 12u
#define SPARK_BIND_CANVAS_OFF 13u
#define SPARK_BIND_MICMUTE 14u
#define SPARK_BIND_RFKILL 15u
#define SPARK_BIND_TABLET_ON 16u
#define SPARK_BIND_TABLET_OFF 17u
#define SPARK_BIND_HEADPHONE_ON 18u
#define SPARK_BIND_HEADPHONE_OFF 19u
#define SPARK_BIND_DOCK_ON 20u
#define SPARK_BIND_DOCK_OFF 21u
#define SPARK_BIND_RESUME 22u
#define SPARK_BIND_EVENTS 22u

/*
        Names in event-id order, the same strings the kernel table uses.
        Userspace matches a typed word against this rather than opening
        /dev/spark once per event. An id is never reused; new events
        append. Two-word names (canvas on) are `moonwater bind canvas on`.
*/
static const char spark_bind_event_name[SPARK_BIND_EVENTS][SPARK_BIND_NAME_MAX] = {
        "poweroff",
        "sleep",
        "reset",
        "ctrl_alt_delete",
        "lid_close",
        "lid_open",
        "volume_up",
        "volume_down",
        "mute",
        "brightness_up",
        "brightness_down",
        "canvas on",
        "canvas off",
        "micmute",
        "rfkill",
        "tablet on",
        "tablet off",
        "headphone on",
        "headphone off",
        "dock on",
        "dock off",
        "resume",
};

static const unsigned char spark_bind_stop[SPARK_BIND_EVENTS] = {
        [SPARK_BIND_POWEROFF - 1] = 1,
        [SPARK_BIND_RESET - 1] = 1,
        [SPARK_BIND_CTRL_ALT_DELETE - 1] = 1,
};

static inline int spark_bind_is_stop(unsigned int event)
{
        return event && event <= SPARK_BIND_EVENTS && spark_bind_stop[event - 1];
}

struct bind_control {
        unsigned int op;     // SPARK_BIND_GET, or SPARK_BIND_SET, which stores command first
        unsigned int event;  // 1 to count
        unsigned int runs;   // answered: runs started since boot
        unsigned int flags;  // answered: SPARK_BIND_*
        unsigned int count;  // answered: how many events there are
        unsigned int reserved[3];
        char name[SPARK_BIND_NAME_MAX];       // answered
        char command[SPARK_BIND_COMMAND_MAX]; // NUL terminated; "" sets the default
};

_Static_assert(sizeof(struct bind_control) == 312, "spark bind ABI");

// _IOWR('s', 11, struct bind_control)
#define SPARK_IOCTL_BIND 0xc138730bu

/*
        One launch request.

        stdio names the child's standard descriptors, and a pipeline stage is
        the reason it is here. Every stage of "a | b | c" is a fresh program
        with a pipe on one side and a pipe on the other, and the shell had no
        way to say so: it forked itself once per stage so the child could
        arrange its own descriptors, and paid a page table copy each time for
        an address space the child discards at exec. Naming them in the
        request lets a stage be spawned rather than forked.

        A descriptor of -1 is left alone, so a stage at either end of the
        pipeline keeps the shell's own. Every end the shell still holds is
        inherited by the child as a copy, the same as a fork, so the shell
        opens pipeline pipes close-on-exec: the three named here are installed
        without that flag and survive, and every other copy goes when the
        image loads. A reader that inherited its own write end would wait for
        an end of file that could never arrive.
*/
struct spawn {
        unsigned long path;       // user pointer, NUL terminated
        unsigned long argv;       // user pointer to the flat argv block
        unsigned int argv_bytes;  // size of that block
        unsigned int argv_count;  // number of strings in it
        unsigned long envp;       // same shape as argv; may be 0 for none
        unsigned int envp_bytes;
        unsigned int envp_count;
        unsigned long envp_generation; // 0 copies; equal nonzero values reuse
        unsigned int flags;       // SPARK_SPAWN_*, none of them required
        int stdio[3];             // stdin, stdout, stderr; -1 leaves one alone
};

// Userspace fills this field by field and the loader reads it in one
// copy_from_user, so the two only agree while the size does.
_Static_assert(sizeof(struct spawn) == 64, "spark spawn request ABI");

/*
        Settings kept inside the boot image.

        The image carries a section of its own, .mwset, that nothing
        compresses: two slots of SPARK_SETTINGS_SLOT bytes, each starting on a
        page of its own. A change is written over the older slot only, in
        place, so the file never changes size and a machine that loses power
        part way has one slot torn and the other whole. The torn one fails its
        sum, or still reads as never written, and the other is what boots.

        The EFI stub copies the section into a configuration table before it
        leaves the firmware, the kernel keeps the newest slot that checks, and
        userspace reads and replaces that copy through /dev/spark. Every
        reader goes through spark_settings_check, so a slot nobody wrote, a
        slot somebody tore and a slot somebody made up are told apart the same
        way in the stub's consumer, the compositor and the shell -- and none of
        them can walk past the end of one.

        Layout, native byte order, which is little-endian on every machine
        this builds for:

                0       magic "MWSETTNG"
                8       version, header size, slot size
                16      generation, highest is newest
                24      medium, sixteen random bytes naming the copy
                40      payload length and CRC-32 over header and payload
                48      flags, and the next id each list hands out
                64      entries: list, kind, id, length, then the text,
                        padded to four bytes; in the bind table the id is
                        the event and the text what runs on it

        A slot whose generation, length, sum and flags are all zero was never
        written: it is the image as built, and means the defaults.
*/
#define SPARK_SETTINGS_MAGIC 0x474e54544553574dull
#define SPARK_SETTINGS_VERSION 1
#define SPARK_SETTINGS_HEADER 64
#define SPARK_SETTINGS_SLOT 16384
#define SPARK_SETTINGS_PAYLOAD (SPARK_SETTINGS_SLOT - SPARK_SETTINGS_HEADER)
#define SPARK_SETTINGS_ENTRY 8
#define SPARK_SETTINGS_TEXT_MOST 4096
#define SPARK_SETTINGS_LIST_MOST 16
#define SPARK_SETTINGS_BIND_MOST 48       // events bound at once
#define SPARK_SETTINGS_BIND_TEXT_MOST 255 // a bound command, without its terminator

//      flags: each is the change from the default, so zero is as built.
#define SPARK_SETTINGS_CANVAS_OFF 0x1u   // Canvas does not start at boot
#define SPARK_SETTINGS_MOUNT_OFF 0x2u    // boot keeps the disks' data unmounted
#define SPARK_SETTINGS_STARTUP_SET 0x4u  // the startup list is the one written

//      lists, and the kinds of entry in them.
#define SPARK_SETTINGS_INIT 1
#define SPARK_SETTINGS_STARTUP 2
#define SPARK_SETTINGS_EXIT 3
#define SPARK_SETTINGS_BIND 4 // event id -> command; no entry is the event's default
#define SPARK_SETTINGS_LISTS 4

#define SPARK_SETTINGS_COMMAND 1       // run through /shell -c
#define SPARK_SETTINGS_SHELL 2         // a terminal window with a shell
#define SPARK_SETTINGS_KERNEL_SHELL 3  // the kernel log window

struct spark_settings {
        unsigned long magic;
        unsigned short version;
        unsigned short header;
        unsigned int slot;
        unsigned long generation;
        unsigned char medium[16];
        unsigned int length;
        unsigned int sum;
        unsigned int flags;
        unsigned short next[SPARK_SETTINGS_LISTS - 1]; // init, startup, exit
        unsigned char reserved[6];
        unsigned char payload[SPARK_SETTINGS_PAYLOAD];
};

struct spark_settings_entry {
        unsigned char list;
        unsigned char kind;
        unsigned short id;     // in the bind table, the event
        unsigned short length;
        unsigned short reserved;
};

_Static_assert(sizeof(struct spark_settings) == SPARK_SETTINGS_SLOT,
               "spark settings slot ABI");
_Static_assert(sizeof(struct spark_settings_entry) == SPARK_SETTINGS_ENTRY,
               "spark settings entry ABI");
_Static_assert(__builtin_offsetof(struct spark_settings, sum) == 44 &&
                   __builtin_offsetof(struct spark_settings, payload) ==
                       SPARK_SETTINGS_HEADER,
               "spark settings header ABI");

#define spark_settings_padded(length) (((unsigned long)(length) + 3) & ~3ul)

//      The sum a sealed slot carries: CRC-32 over its header with the sum field
//      read as zero, then exactly length bytes of payload -- library.c's
//      hash_crc32, which the kernel and the shell both read before this file,
//      chained the way the moonwater command seals a slot.
static inline unsigned int spark_settings_sum(const struct spark_settings *slot)
{
        const unsigned char *bytes = (const unsigned char *)slot;
        unsigned int crc = hash_crc32(~0u, (void *)bytes, 44);

        crc = hash_crc32(crc, (void *)"\0\0\0", 4);
        crc = hash_crc32(crc, (void *)(bytes + 48), SPARK_SETTINGS_HEADER - 48);
        return ~hash_crc32(crc, (void *)slot->payload, slot->length);
}

/*
        0 for a slot never written, 1 for one whose settings check, -1 for
        anything else: another format, a torn write, a length past the slot, a
        sum that disagrees, or entries that do not walk exactly to the end
        within their limits.
*/
static inline int spark_settings_check(const struct spark_settings *slot)
{
        unsigned long counts[SPARK_SETTINGS_LISTS + 1] = {0};
        unsigned long at = 0;

        if (slot->magic != SPARK_SETTINGS_MAGIC ||
            slot->version != SPARK_SETTINGS_VERSION ||
            slot->header != SPARK_SETTINGS_HEADER ||
            slot->slot != SPARK_SETTINGS_SLOT)
                return -1;

        if (!slot->generation && !slot->length && !slot->sum && !slot->flags)
                return 0;

        if (slot->length > SPARK_SETTINGS_PAYLOAD ||
            spark_settings_sum(slot) != slot->sum)
                return -1;

        while (at < slot->length)
        {
                const unsigned char *entry = slot->payload + at;
                unsigned int list;
                unsigned int length;

                if (slot->length - at < SPARK_SETTINGS_ENTRY)
                        return -1;

                list = entry[0];
                length = entry[4] | (unsigned int)entry[5] << 8;
                at += SPARK_SETTINGS_ENTRY;

                if (!list || list > SPARK_SETTINGS_LISTS ||
                    ++counts[list] > (list == SPARK_SETTINGS_BIND
                                          ? SPARK_SETTINGS_BIND_MOST
                                          : SPARK_SETTINGS_LIST_MOST) ||
                    length > (list == SPARK_SETTINGS_BIND
                                  ? SPARK_SETTINGS_BIND_TEXT_MOST
                                  : SPARK_SETTINGS_TEXT_MOST) ||
                    slot->length - at < spark_settings_padded(length))
                        return -1;

                at += spark_settings_padded(length);
        }

        return 1;
}

/*
        Which of two slots to believe: the newest that checks, a slot never
        written counting as generation zero. -1 when neither does, which reads
        as the defaults.
*/
static inline int spark_settings_newest(const struct spark_settings *slots)
{
        int first = spark_settings_check(slots);
        int second = spark_settings_check(slots + 1);

        if (first < 0)
                return second < 0 ? -1 : 1;
        if (second < 0)
                return 0;

        return slots[1].generation > slots[0].generation ? 1 : 0;
}

/*
        The settings the kernel holds: the newest slot the image booted with,
        or the last one set since. Both requests are root's, because a command
        can carry a secret. GET answers ENODATA when the image handed over
        nothing and nothing was set. SET checks the slot the way boot does and
        replaces the session copy; moonwater boot applies bind rows from the
        image. Nothing here opens or closes Canvas.
*/
struct spark_settings_request {
        unsigned long address; // user pointer to SPARK_SETTINGS_SLOT bytes
        unsigned long flags;   // none defined; nonzero is refused
};

// _IOR('s', 12, struct spark_settings_request)
#define SPARK_IOCTL_SETTINGS_GET 0x8010730cu
// _IOW('s', 13, struct spark_settings_request)
#define SPARK_IOCTL_SETTINGS_SET 0x4010730du

_Static_assert(sizeof(struct spark_settings_request) == 16,
               "spark settings request ABI");

#endif
