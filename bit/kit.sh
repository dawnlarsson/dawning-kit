#!/bin/bash
#
#       Dawning Bit Kit
#       Provides foundational primitives for code generation in a bare bones UNIX environment.
#       "starting from nothing"
#
#       Usage: . bit/kit.sh
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-devkit
#

BIG_ENDIAN=${BIG_ENDIAN:-0}

is_hex() {
        case "$1" in
        0x*) return 0 ;;
        *) return 1 ;;
        esac
}

is_decimal() {
        case "$1" in
        '' | *[!0-9]*) return 1 ;;
        *) return 0 ;;
        esac
}

is_quoted() {
        case "$1" in
        \"*\") return 0 ;;
        *) return 1 ;;
        esac
}

is_alpha() {
        case "$1" in
        '' | *[!a-zA-Z]*) return 1 ;;
        *) return 0 ;;
        esac
}

emit_hex_bytes() {
        val="$1"
        width="$2" # 4, 8, 16, 32 chars

        if [ "$BIG_ENDIAN" -eq 1 ]; then
                i=0
                while [ "$i" -lt "$width" ]; do
                        byte="${val#"${val%??}"}"
                        val="${val%??}"
                        printf "\\x$byte"
                        i=$((i + 2))
                done
        else
                while [ ${#val} -gt 0 ]; do
                        byte="${val#"${val%??}"}"
                        val="${val%??}"
                        printf "\\x$byte"
                done
        fi
}

bit_8() {
        for arg in "$@"; do
                arg="${arg%,}"

                if is_hex "$arg"; then
                        printf "\\x${arg#0x}"
                elif is_quoted "$arg"; then
                        str="${arg#\"}"
                        str="${str%\"}"
                        printf "%s" "$str"
                elif is_alpha "$arg"; then
                        printf "%s" "$arg"
                elif is_decimal "$arg"; then
                        printf "\\x$(printf "%02x" "$arg")"
                fi
        done
}

bit_16() {
        for arg in "$@"; do
                arg="${arg%,}"

                if is_hex "$arg"; then
                        val="${arg#0x}"
                        val=$(printf "%04x" "0x$val")
                elif is_decimal "$arg"; then
                        val=$(printf "%04x" "$arg")
                else
                        continue
                fi

                emit_hex_bytes "$val" 4
        done
}

bit_32() {
        for arg in "$@"; do
                arg="${arg%,}"

                if is_hex "$arg"; then
                        val="${arg#0x}"
                        val=$(printf "%08x" "0x$val")
                elif is_decimal "$arg"; then
                        val=$(printf "%08x" "$arg")
                else
                        continue
                fi

                emit_hex_bytes "$val" 8
        done
}

bit_64() {
        for arg in "$@"; do
                arg="${arg%,}"

                if is_hex "$arg"; then
                        val="${arg#0x}"
                        val=$(printf "%016x" "0x$val")
                elif is_decimal "$arg"; then
                        val=$(printf "%016x" "$arg")
                else
                        continue
                fi

                emit_hex_bytes "$val" 16
        done
}

bit128() {
        for arg in "$@"; do
                arg="${arg%,}"

                if is_hex "$arg"; then
                        val="${arg#0x}"
                        val=$(printf "%032x" "0x$val")
                elif is_decimal "$arg"; then
                        val=$(printf "%032x" "$arg")
                else
                        continue
                fi

                emit_hex_bytes "$val" 32
        done
}

elf() {
        output="$1"
        code_generator="$2"

        code_section="/tmp/code_section_$$"
        $code_generator >"$code_section"
        code_size=$(wc -c <"$code_section")

        ELF_OFFSET=65536
        ELF_HEADER_SIZE=64
        PROGRAM_HEADER_SIZE=56
        ENTRY_OFFSET=$((ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE))
        TOTAL_SIZE=$((ENTRY_OFFSET + code_size))

        {
                # ELF Header (64 bytes)
                bit_8 0x7f, "ELF", 2, 1, 1, 0
                bit_64 0                              # padding
                bit_16 2                              # e_type: ET_EXEC
                bit_16 0x3e                           # e_machine: EM_X86_64
                bit_32 1                              # e_version
                bit_64 $((ENTRY_OFFSET + ELF_OFFSET)) # e_entry
                bit_64 $ELF_HEADER_SIZE               # e_phoff
                bit_64 0                              # e_shoff
                bit_32 0                              # e_flags
                bit_16 $ELF_HEADER_SIZE               # e_ehsize
                bit_16 $PROGRAM_HEADER_SIZE           # e_phentsize
                bit_16 1                              # e_phnum
                bit_16 64                             # e_shentsize
                bit_16 0                              # e_shnum
                bit_16 0                              # e_shstrndx

                # Program Header (56 bytes)
                bit_32 1           # p_type: PT_LOAD
                bit_32 7           # p_flags: PF_R | PF_W | PF_X
                bit_64 0           # p_offset
                bit_64 $ELF_OFFSET # p_vaddr
                bit_64 $ELF_OFFSET # p_paddr
                bit_64 $TOTAL_SIZE # p_filesz
                bit_64 $TOTAL_SIZE # p_memsz
                bit_64 0x1000      # p_align

                cat "$code_section"

        } >"$output"

        chmod +x "$output"
        rm -f "$code_section"
}

# variable-length integer encoding
wasm_var() {
        local value="$1"
        while [ $value -ge 128 ]; do
                bit_8 $((value & 0x7F | 0x80))
                value=$((value >> 7))
        done
        bit_8 $((value & 0x7F))
}

wasm_section() {
        local section_id="$1"
        local content_generator="$2"

        local temp_section="/tmp/wasm_section_$$"
        $content_generator >"$temp_section"
        local section_size=$(wc -c <"$temp_section")

        bit_8 $section_id
        wasm_var $section_size
        cat "$temp_section"
        rm -f "$temp_section"
}

wasm() {
        local output="$1"
        local code_generator="$2"

        local code_section="/tmp/wasm_code_section"
        $code_generator >"$code_section"

        {
                # WASM header
                bit_8 0x00, 0x61, 0x73, 0x6d # magic
                bit_32 0x01                  # version

                # User's code generator output
                cat "$code_section"

        } >"$output"

        chmod +x "$output"
        rm -f "$code_section"
}
