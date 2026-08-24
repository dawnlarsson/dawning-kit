/*
        Dawning spark binary format

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

#ifndef DAWNING_SPARK_H
#define DAWNING_SPARK_H

// "SPRK", little endian
#define SPARK_MAGIC 0x4b525053u

#define SPARK_VERSION 1
#define SPARK_PAGE 4096

// Anything below this in the file cannot be a valid image. The kernel
// pre-reads BINPRM_BUF_SIZE (256) bytes for us, so the header must fit there.
#define SPARK_HEADER_SIZE 64

struct spark_header {
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

#endif
