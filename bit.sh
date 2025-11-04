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
# shellcheck disable=SC2059

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
        \"*\") 
                case "$1" in
                *\") return 0 ;;
                *) return 1 ;;
                esac
                ;;
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
                        byte="${val%"${val#??}"}"
                        val="${val#??}"
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

hex_dump() {
        func_name="$1"

        hex_output=$($func_name | od -tx1 -An | tr -d '\n')

        byte_count=0
        for hex_byte in $hex_output; do
                byte_count=$((byte_count + 1))
        done

        printf "%s %s\n" "$func_name" "$byte_count bytes"

        hex_data=$($func_name | od -tx1 -An)

        offset=0
        echo "$hex_data" | {
                while read -r line; do
                        if [ -n "$line" ]; then
                                printf "%08x   " $offset

                                printf "%-48s" "$line"

                                ascii=""
                                for hex_byte in $line; do
                                        ascii_val=$(printf "%d" "0x$hex_byte")
                                        if [ "$ascii_val" -ge 32 ] && [ "$ascii_val" -le 126 ]; then
                                                ascii_char=$(printf "\\$(printf '%03o' "$ascii_val")")
                                                ascii="$ascii$ascii_char"
                                        else
                                                ascii="$ascii."
                                        fi
                                done

                                printf "  %s\n" "$ascii"
                                offset=$((offset + 16))
                        fi
                done
        }
}

elf() {
        output="$1"
        code_generator="$2"
        
        if [ -z "$output" ] || [ -z "$code_generator" ]; then
                echo "Error: missing arguments" >&2
                return 1
        fi

        code_section="/tmp/code_section_$$"
        trap 'rm -f "$code_section"' EXIT INT TERM
        
        if ! $code_generator >"$code_section" 2>/dev/null; then
                echo "Error: code generator failed" >&2
                rm -f "$code_section"
                return 1
        fi
        
        code_size=$(wc -c <"$code_section")

        ELF_OFFSET=65536
        ELF_HEADER_SIZE=64
        PROGRAM_HEADER_SIZE=56
        ENTRY_OFFSET=$((ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE))
        TOTAL_SIZE=$((ENTRY_OFFSET + code_size))

        {
                # ELF Header (64 bytes)
                bit_8 0x7f, "ELF", 2, 1, 1, 0
                bit_64 0
                bit_16 2
                bit_16 0x3e
                bit_32 1
                bit_64 $((ENTRY_OFFSET + ELF_OFFSET))
                bit_64 $ELF_HEADER_SIZE
                bit_64 0
                bit_32 0
                bit_16 $ELF_HEADER_SIZE
                bit_16 $PROGRAM_HEADER_SIZE
                bit_16 1
                bit_16 64
                bit_16 0
                bit_16 0

                # Program Header (56 bytes)
                bit_32 1
                bit_32 7
                bit_64 0
                bit_64 $ELF_OFFSET
                bit_64 $ELF_OFFSET
                bit_64 $TOTAL_SIZE
                bit_64 $TOTAL_SIZE
                bit_64 0x1000

                cat "$code_section"

        } >"$output" || { echo "Error: failed to write output" >&2; rm -f "$code_section"; return 1; }

        chmod +x "$output" || { echo "Error: failed to set executable" >&2; rm -f "$code_section"; return 1; }
        rm -f "$code_section"
        trap - EXIT INT TERM
}

# variable-length integer encoding
wasm_var() {
        local value="$1"
        while [ "$value" -ge 128 ]; do
                bit_8 $((value & 0x7F | 0x80))
                value=$((value >> 7))
        done
        bit_8 $((value & 0x7F))
}

wasm_svar() {
        local value="$1"
        local more=1
        
        while [ $more -eq 1 ]; do
                local byte=$((value & 0x7F))
                value=$((value >> 7))
                
                if { [ $value -eq 0 ] && [ $((byte & 0x40)) -eq 0 ]; } || \
                   { [ $value -eq -1 ] && [ $((byte & 0x40)) -ne 0 ]; }; then
                        more=0
                else
                        byte=$((byte | 0x80))
                fi
                
                bit_8 $byte
        done
}

wasm_section() {
        local section_id="$1"
        local content_generator="$2"

        [ -z "$content_generator" ] && { echo "Error: missing content generator" >&2; return 1; }

        local temp_section="/tmp/wasm_section_$$"
        local section_size
        trap 'rm -f "$temp_section"' EXIT INT TERM
        
        $content_generator >"$temp_section" || { rm -f "$temp_section"; return 1; }
        section_size=$(wc -c <"$temp_section")

        bit_8 "$section_id"
        wasm_var "$section_size"
        cat "$temp_section"
        rm -f "$temp_section"
        trap - EXIT INT TERM
}

wasm() {
        local output="$1"
        local code_generator="$2"

        if [ -z "$output" ] || [ -z "$code_generator" ]; then
                echo "Error: missing arguments" >&2
                return 1
        fi

        local code_section="/tmp/code_section_$$"
        trap 'rm -f "$code_section"' EXIT INT TERM
        
        $code_generator >"$code_section" || { rm -f "$code_section"; return 1; }

        {
                bit_8 0x00, 0x61, 0x73, 0x6d
                bit_32 0x01
                cat "$code_section"
        } >"$output" || { rm -f "$code_section"; return 1; }

        chmod +x "$output" || { rm -f "$code_section"; return 1; }
        rm -f "$code_section"
        trap - EXIT INT TERM
}