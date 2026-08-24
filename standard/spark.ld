/*
        Linker script for the spark format.

        The loader maps text, data and bss as three separate regions with
        different permissions, so each one has to start on a page boundary --
        otherwise two regions share a page and cannot be given different
        protections. Aligning here is what keeps the kernel side free of
        partial page fixups.

        SPARK_BASE has to match the base recorded in the spark header. The
        image carries absolute addresses, so it is mapped MAP_FIXED there.
*/

SPARK_BASE = 0x400000;

ENTRY(_start)

SECTIONS
{
        . = SPARK_BASE;

        .text : {
                *(.text .text.*)
                *(.rodata .rodata.*)
        }

        . = ALIGN(4096);

        .data : {
                *(.data .data.*)
                *(.got .got.plt)
        }

        . = ALIGN(4096);

        .bss : {
                __bss_start = .;
                *(.bss .bss.*)
                *(COMMON)
                . = ALIGN(4096);
                __bss_end = .;
        }

        /* Nothing else is loadable: the format has no section table, no
           dynamic linking and no unwind information. */
        /DISCARD/ : {
                *(.comment)
                *(.note .note.*)
                *(.eh_frame .eh_frame_hdr)
                *(.gnu.hash .hash .dynsym .dynstr)
        }
}
