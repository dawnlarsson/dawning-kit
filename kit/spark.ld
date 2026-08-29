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
        /* The spark header lives in the first 64 bytes of the mapped image,
           so the code starts just past it. The loader maps from SPARK_BASE and
           jumps to the entry recorded in the header, which points here. */
        . = SPARK_BASE + 64;

        .text : {
                *(.text .text.*)
                *(.rodata .rodata.*)
                /* riscv64 puts read only globals of a few bytes here. */
                *(.srodata .srodata.*)
        }

        . = ALIGN(4096);

        .data : {
                *(.data .data.*)
                *(.got .got.plt)
                /* And small writable ones here, one section each because the
                   programs are built with -fdata-sections. Unmatched, they
                   were placed wherever the linker felt like, which on riscv64
                   is outside every region the loader maps. */
                *(.sdata .sdata.*)
        }

        /* RISC-V _start establishes gp as required by its psABI.  Ordinary
           ELF linker scripts provide this anchor themselves; spark owns its
           complete layout and therefore has to provide it explicitly.  The
           conventional 0x800 bias reaches the full signed twelve-bit window
           around small data.  Other architectures simply leave it unused. */
        PROVIDE(__global_pointer$ = . + 0x800);

        . = ALIGN(4096);

        .bss : {
                __bss_start = .;
                *(.bss .bss.*)
                *(.sbss .sbss.*)
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
