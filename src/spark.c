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
#define SPARK_IOCTL_SPAWN 0x40307301u

// Same request, with the shell's ENOEXEC rule: executable text without a #!
// line is handed to /bin/sh. Kept separate so raw spawn remains an exact
// execve-like interface and callers that do not want shell interpretation do
// not acquire it accidentally.
#define SPARK_IOCTL_SPAWN_SHELL 0x40307304u

// Run the argv[0] utility in the immutable system /shell image.
#define SPARK_IOCTL_SPAWN_TOOL 0x40307307u

// The same launch with stdout and stderr installed before exec.
#define SPARK_IOCTL_SPAWN_TOOL_TO 0x40387308u

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

struct spawn {
        unsigned long path;       // user pointer, NUL terminated
        unsigned long argv;       // user pointer to the flat argv block
        unsigned int argv_bytes;  // size of that block
        unsigned int argv_count;  // number of strings in it
        unsigned long envp;       // same shape as argv; may be 0 for none
        unsigned int envp_bytes;
        unsigned int envp_count;
        unsigned long envp_generation; // 0 copies; equal nonzero values reuse
};

struct spawn_to {
        struct spawn spawn;
        int output;
        int error;
};

#endif
