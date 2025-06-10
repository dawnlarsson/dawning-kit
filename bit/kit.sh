#!/bin/bash
#
#       Dawning Bit Kit
#       Provides foundational primitives for code generation in a bare bones UNIX environment.
#       "starting from nothing"
#
#       Usage: source bit/kit.sh
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-devkit
#

BIG_ENDIAN=${BIG_ENDIAN:-0}

# Define 8 bits (1 byte)
bit__8() {
        for arg in "$@"; do
                arg="${arg%,}"
                if [[ "$arg" =~ ^0x[0-9a-fA-F]+$ ]]; then
                        printf "\\x${arg#0x}"
                elif [[ "$arg" =~ ^\".*\"$ ]]; then
                        local str="${arg#\"}"
                        str="${str%\"}"
                        echo -n "$str"
                elif [[ "$arg" =~ ^[a-zA-Z]+$ ]]; then
                        echo -n "$arg"
                elif [[ "$arg" =~ ^[0-9]+$ ]]; then
                        printf "\\x$(printf "%02x" "$arg")"
                fi
        done
}

# Define 16 bits (2 bytes) - little endian default
# Also known as word or short
bit_16() {
        for arg in "$@"; do
                arg="${arg%,}"
                if [[ "$arg" =~ ^0x[0-9a-fA-F]+$ ]]; then
                        local val="${arg#0x}"
                        val=$(printf "%04x" "0x$val")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                # Big endian: most significant byte first
                                printf "\\x${val:0:2}\\x${val:2:2}"
                        else
                                # Little endian: least significant byte first
                                printf "\\x${val:2:2}\\x${val:0:2}"
                        fi
                elif [[ "$arg" =~ ^[0-9]+$ ]]; then
                        local val=$(printf "%04x" "$arg")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                printf "\\x${val:0:2}\\x${val:2:2}"
                        else
                                printf "\\x${val:2:2}\\x${val:0:2}"
                        fi
                fi
        done
}

# Define 32 bits (4 bytes) - little endian default
# Also known as double word or int
bit_32() {
        for arg in "$@"; do
                arg="${arg%,}"
                if [[ "$arg" =~ ^0x[0-9a-fA-F]+$ ]]; then
                        local val="${arg#0x}"
                        val=$(printf "%08x" "0x$val")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                # Big endian: bytes 0,1,2,3
                                for ((i = 0; i <= 6; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                # Little endian: bytes 3,2,1,0
                                for ((i = 6; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                elif [[ "$arg" =~ ^[0-9]+$ ]]; then
                        local val=$(printf "%08x" "$arg")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                for ((i = 0; i <= 6; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                for ((i = 6; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                fi
        done
}

# Define 64 bits (8 bytes) - little endian default
# Also known as quad word or long
bit_64() {
        for arg in "$@"; do
                arg="${arg%,}"
                if [[ "$arg" =~ ^0x[0-9a-fA-F]+$ ]]; then
                        local val="${arg#0x}"
                        val=$(printf "%016x" "0x$val")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                # Big endian: bytes 0,1,2,3,4,5,6,7
                                for ((i = 0; i <= 14; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                # Little endian: bytes 7,6,5,4,3,2,1,0
                                for ((i = 14; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                elif [[ "$arg" =~ ^[0-9]+$ ]]; then
                        local val=$(printf "%016x" "$arg")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                for ((i = 0; i <= 14; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                for ((i = 14; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                fi
        done
}

# Defines 128 bits (16 bytes) - little endian default
# Also known as quad double word
bit_128() {
        for arg in "$@"; do
                arg="${arg%,}"
                if [[ "$arg" =~ ^0x[0-9a-fA-F]+$ ]]; then
                        local val="${arg#0x}"
                        val=$(printf "%032x" "0x$val")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                # Big endian: bytes 0-15
                                for ((i = 0; i <= 30; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                # Little endian: bytes 15-0
                                for ((i = 30; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                elif [[ "$arg" =~ ^[0-9]+$ ]]; then
                        local val=$(printf "%032x" "$arg")
                        if [[ $BIG_ENDIAN -eq 1 ]]; then
                                for ((i = 0; i <= 30; i += 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        else
                                for ((i = 30; i >= 0; i -= 2)); do
                                        printf "\\x${val:$i:2}"
                                done
                        fi
                fi
        done
}

elf() {
        local output="$1"
        local code_generator="$2"

        local code_section="/tmp/code_section"
        $code_generator >"$code_section"
        local code_size=$(wc -c <"$code_section")

        local ELF_OFFSET=$((2048 * 32))
        local ELF_HEADER_SIZE=64
        local PROGRAM_HEADER_SIZE=56
        local ENTRY_OFFSET=$((ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE))
        local TOTAL_SIZE=$((ENTRY_OFFSET + code_size))

        {
                # ELF Header (64 bytes)
                bit__8 0x7f, ELF, 2, 1, 1, 0          # e_ident
                bit_64 0                              # padding
                bit_16 2                              # e_type: ET_EXEC
                bit_16 0x3e                           # e_machine: EM_X86_64
                bit_32 1                              # e_version
                bit_64 $((ENTRY_OFFSET + ELF_OFFSET)) # e_entry: entry point
                bit_64 $ELF_HEADER_SIZE               # e_phoff: program header offset
                bit_64 0                              # e_shoff: no section headers
                bit_32 0                              # e_flags
                bit_16 $ELF_HEADER_SIZE               # e_ehsize: ELF header size
                bit_16 $PROGRAM_HEADER_SIZE           # e_phentsize: program header size
                bit_16 1                              # e_phnum: 1 program header
                bit_16 64                             # e_shentsize: section header size (unused)
                bit_16 0                              # e_shnum: no section headers
                bit_16 0                              # e_shstrndx: no string table

                # Program Header (56 bytes)
                bit_32 1           # p_type: PT_LOAD
                bit_32 7           # p_flags: PF_R | PF_W | PF_X
                bit_64 0           # p_offset: start of file
                bit_64 $ELF_OFFSET # p_vaddr: virtual address
                bit_64 $ELF_OFFSET # p_paddr: physical address
                bit_64 $TOTAL_SIZE # p_filesz: size in file
                bit_64 $TOTAL_SIZE # p_memsz: size in memory
                bit_64 0x1000      # p_align: page alignment

                cat "$code_section"

        } >"$output"

        chmod +x "$output"
        rm -f "$code_section"
}
