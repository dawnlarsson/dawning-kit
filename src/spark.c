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
// this changes if the request struct does.
#define SPARK_IOCTL_SPAWN 0x40287301u

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

struct spawn {
        unsigned long path;       // user pointer, NUL terminated
        unsigned long argv;       // user pointer to the flat argv block
        unsigned int argv_bytes;  // size of that block
        unsigned int argv_count;  // number of strings in it
        unsigned long envp;       // same shape as argv; may be 0 for none
        unsigned int envp_bytes;
        unsigned int envp_count;
};

#endif
