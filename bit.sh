#!/bin/sh
#
#       Dawning Bit Kit
#       Provides foundational primitives for code generation in a bare bones UNIX environment.
#       "starting from nothing"
#
#       Usage: . bit.sh
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-kit
#
# shellcheck disable=SC2059
ENDIAN=${ENDIAN:-0}

# Set BIT_QUIET=1 to silence range/truncation warnings.
BIT_QUIET=${BIT_QUIET:-0}

bit_warn() {
        [ "$BIT_QUIET" = 1 ] && return 0
        printf 'bit: %s\n' "$*" >&2
        return 0
}

is_hex() {
        case "$1" in
        0x*) return 0 ;;
        *) return 1 ;;
        esac
}

# Accepts an optional leading '-'; negatives are encoded two's complement.
is_decimal() {
        case "$1" in
        '' | '-') return 1 ;;
        -*) case "${1#-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac ;;
        *[!0-9]*) return 1 ;;
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

# Emits raw bytes for decimal byte values passed as arguments.
# \xNN is a bash extension that dash's printf emits literally, so this goes
# through %b with POSIX octal escapes instead.
emit_bytes() {
        [ $# -eq 0 ] && return 0
        printf '%b' "$(printf '\\0%03o' "$@")"
}

# Emits exactly `width`/2 bytes from a hex string of exactly `width` chars.
# Both endian paths honour `width`, so a call always produces a fixed length.
emit_hex_bytes() {
        val="$1"
        width="$2"

        # Normalize to exactly `width` hex chars: pad short, truncate long from the left.
        while [ ${#val} -lt "$width" ]; do
                val="0$val"
        done
        while [ ${#val} -gt "$width" ]; do
                val="${val#?}"
        done

        set --
        case "$ENDIAN" in
        big | Big | BIG | BE | be | 1)
                while [ ${#val} -gt 0 ]; do
                        byte="${val%"${val#??}"}"
                        val="${val#??}"
                        set -- "$@" $((0x$byte))
                done
                ;;
        *)
                while [ ${#val} -gt 0 ]; do
                        byte="${val#"${val%??}"}"
                        val="${val%??}"
                        set -- "$@" $((0x$byte))
                done
                ;;
        esac

        emit_bytes "$@"
}

# Converts one argument to a masked hex string of `hex_width` chars.
# Warns (once per offending value) when the value did not fit.
# Returns 1 when the argument is not numeric, so callers can skip it.
bit_value() {
        _arg="$1"
        _hex_width="$2"
        _bytes=$((_hex_width / 2))

        if is_hex "$_arg"; then
                # Validate before arithmetic: a bad constant aborts the shell in dash.
                case "${_arg#0x}" in
                '' | *[!0-9a-fA-F]*) return 1 ;;
                esac
                _num=$((_arg))
        elif is_decimal "$_arg"; then
                _num=$((_arg))
        else
                return 1
        fi

        if [ "$_bytes" -lt 8 ]; then
                _mask=$(((1 << (_bytes * 8)) - 1))
                _masked=$((_num & _mask))

                # Negative input is intentional two's complement, not an overflow.
                if [ "$_masked" -ne "$_num" ] && [ "$_num" -ge 0 ]; then
                        bit_warn "warning: $_arg does not fit in $_bytes byte(s), truncated to $(printf '0x%x' "$_masked")"
                fi
                _num=$_masked
        fi

        printf "%0${_hex_width}x" "$_num"
}

# Applies `$1` to each argument, splitting comma separated groups so both
# `bit_8 0x7f, 0x45` and `bit_8 0x7f,0x45` behave identically.
# Quoted arguments are passed through whole, commas included.
bit_each() {
        _fn="$1"
        shift

        for _a in "$@"; do
                case "$_a" in
                \"*\")
                        "$_fn" "$_a"
                        continue
                        ;;
                esac

                while :; do
                        case "$_a" in
                        *,*)
                                _f="${_a%%,*}"
                                _a="${_a#*,}"
                                ;;
                        *)
                                _f="$_a"
                                _a=""
                                ;;
                        esac

                        [ -n "$_f" ] && "$_fn" "$_f"
                        [ -z "$_a" ] && break
                done
        done
}

bit_8_one() {
        arg="$1"

        if is_quoted "$arg"; then
                str="${arg#\"}"
                str="${str%\"}"
                printf "%s" "$str"
        elif is_hex "$arg" || is_decimal "$arg"; then
                val=$(bit_value "$arg" 2) || {
                        bit_warn "warning: bit_8 ignoring malformed number '$arg'"
                        return 0
                }
                emit_bytes $((0x$val))
        elif is_alpha "$arg"; then
                printf "%s" "$arg"
        else
                bit_warn "warning: bit_8 ignoring unrecognized argument '$arg'"
        fi
}

bit_16_one() {
        val=$(bit_value "$1" 4) || {
                bit_warn "warning: bit_16 ignoring non-numeric argument '$1'"
                return 0
        }
        emit_hex_bytes "$val" 4
}

bit_32_one() {
        val=$(bit_value "$1" 8) || {
                bit_warn "warning: bit_32 ignoring non-numeric argument '$1'"
                return 0
        }
        emit_hex_bytes "$val" 8
}

bit_64_one() {
        val=$(bit_value "$1" 16) || {
                bit_warn "warning: bit_64 ignoring non-numeric argument '$1'"
                return 0
        }
        emit_hex_bytes "$val" 16
}

bit_8() { bit_each bit_8_one "$@"; }
bit_16() { bit_each bit_16_one "$@"; }
bit_32() { bit_each bit_32_one "$@"; }
bit_64() { bit_each bit_64_one "$@"; }
bit128() { bit_each bit128_one "$@"; }

# 128-bit values exceed shell arithmetic, so hex is handled as a string.
bit128_one() {
        arg="$1"

        if is_hex "$arg"; then
                val="${arg#0x}"
                case "$val" in
                '' | *[!0-9a-fA-F]*)
                        bit_warn "warning: bit128 ignoring malformed hex '$arg'"
                        return 0
                        ;;
                esac
                if [ ${#val} -gt 32 ]; then
                        bit_warn "warning: $arg does not fit in 16 bytes, truncated to the low 128 bits"
                fi
        elif is_decimal "$arg"; then
                # Shell arithmetic is 64-bit; sign-extend into the upper half.
                val=$(printf "%016x" $((arg)))
                case "$arg" in
                -*) val="ffffffffffffffff$val" ;;
                *) val="0000000000000000$val" ;;
                esac
        else
                bit_warn "warning: bit128 ignoring non-numeric argument '$arg'"
                return 0
        fi

        emit_hex_bytes "$val" 32
}

hex_dump() {
        func_name="$1"

        if [ -z "$func_name" ]; then
                echo "hex_dump: missing generator function" >&2
                return 1
        fi

        hex_data=$($func_name | od -tx1 -An) || return 1
        byte_count=$(printf '%s' "$hex_data" | tr -d ' \n')
        byte_count=$((${#byte_count} / 2))

        printf "%s %d bytes\n" "$func_name" "$byte_count"

        offset=0
        printf '%s\n' "$hex_data" | {
                while read -r line; do
                        if [ -n "$line" ]; then
                                printf "%08x   " "$offset"
                                printf "%-48s" "$line"

                                ascii=""
                                # shellcheck disable=SC2086
                                set -- $line
                                for hex_byte in "$@"; do
                                        if [ -n "$hex_byte" ]; then
                                                ascii_val=$(printf "%d" "0x$hex_byte" 2>/dev/null) || ascii_val=""
                                                if [ -n "$ascii_val" ]; then
                                                        if [ "$ascii_val" -ge 32 ] && [ "$ascii_val" -le 126 ]; then
                                                                ascii_char=$(printf "\\$(printf '%03o' "$ascii_val")")
                                                                ascii="$ascii$ascii_char"
                                                        else
                                                                ascii="$ascii."
                                                        fi
                                                fi
                                        fi
                                done

                                printf "  %s\n" "$ascii"
                                offset=$((offset + 16))
                        fi
                done
        }
}

# Creates a private temp file. Falls back to a $$-based path only if mktemp
# is unavailable, in which case O_EXCL-ish semantics are approximated.
bit_tempfile() {
        if command -v mktemp >/dev/null 2>&1; then
                mktemp "${TMPDIR:-/tmp}/dawning_bit.XXXXXXXXXX" && return 0
        fi

        _fallback="${TMPDIR:-/tmp}/dawning_bit_$$_$1"
        [ -e "$_fallback" ] && rm -f "$_fallback"
        (umask 077 && : >"$_fallback") || return 1
        printf '%s' "$_fallback"
}

# ELF machine id for a normalized architecture name.
bit_elf_machine() {
        case "$1" in
        x86_64 | amd64 | x64) echo 62 ;;  # EM_X86_64
        aarch64 | arm64) echo 183 ;;      # EM_AARCH64
        riscv64 | riscv) echo 243 ;;      # EM_RISCV
        *) return 1 ;;
        esac
}

# elf <output> <generator> [arch]
# arch defaults to x86_64 so existing callers keep their target; pass aarch64
# or riscv64 explicitly. The generator must emit code for the same arch.
elf() {
        output="$1"
        code_generator="$2"
        elf_arch="${3:-x86_64}"

        if [ -z "$output" ] || [ -z "$code_generator" ]; then
                echo "elf: usage: elf <output> <generator> [arch]" >&2
                return 1
        fi

        if ! machine=$(bit_elf_machine "$elf_arch"); then
                echo "elf: unsupported architecture '$elf_arch' (x86_64, aarch64, riscv64)" >&2
                return 1
        fi

        code_section=$(bit_tempfile elf) || {
                echo "elf: could not create temp file" >&2
                return 1
        }

        if ! $code_generator >"$code_section"; then
                echo "elf: code generator '$code_generator' failed" >&2
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
                bit_16 "$machine"
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
                bit_32 5 # PF_R | PF_X -- never map the segment writable
                bit_64 0
                bit_64 $ELF_OFFSET
                bit_64 $ELF_OFFSET
                bit_64 $TOTAL_SIZE
                bit_64 $TOTAL_SIZE
                bit_64 0x1000

                cat "$code_section"

        } >"$output" || {
                echo "elf: failed to write $output" >&2
                rm -f "$code_section"
                return 1
        }

        rm -f "$code_section"

        chmod +x "$output" || {
                echo "elf: failed to mark $output executable" >&2
                return 1
        }
}

# variable-length integer encoding
wasm_var() {
        value="$1"
        while [ "$value" -ge 128 ]; do
                bit_8 $((value & 0x7F | 0x80))
                value=$((value >> 7))
        done
        bit_8 $((value & 0x7F))
}

wasm_svar() {
        value="$1"
        more=1

        while [ $more -eq 1 ]; do
                byte=$((value & 0x7F))
                value=$((value >> 7))

                if { [ $value -eq 0 ] && [ $((byte & 0x40)) -eq 0 ]; } ||
                        { [ $value -eq -1 ] && [ $((byte & 0x40)) -ne 0 ]; }; then
                        more=0
                else
                        byte=$((byte | 0x80))
                fi

                bit_8 $byte
        done
}

# Emits a generator's output prefixed with its LEB128 byte length.
# Function bodies inside the code section need exactly this framing, and the
# body must itself begin with a local-declaration count.
# A section generator may itself call wasm_body (a code section contains
# length-prefixed function bodies), so the per-call state is kept in
# depth-indexed variables rather than plain globals.
wasm_body() {
        [ -z "$1" ] && {
                echo "wasm_body: missing content generator" >&2
                return 1
        }

        _wasm_depth=$((${_wasm_depth:-0} + 1))

        _wb_tmp=$(bit_tempfile wasm) || {
                echo "wasm_body: could not create temp file" >&2
                _wasm_depth=$((_wasm_depth - 1))
                return 1
        }

        eval "_wb_file_$_wasm_depth=\$_wb_tmp"
        eval "_wb_name_$_wasm_depth=\$1"

        if $1 >"$_wb_tmp"; then
                _wb_status=0
        else
                _wb_status=1
        fi

        # A nested wasm_body restores _wasm_depth, so this is still our slot.
        eval "_wb_tmp=\$_wb_file_$_wasm_depth"
        eval "_wb_name=\$_wb_name_$_wasm_depth"
        _wasm_depth=$((_wasm_depth - 1))

        if [ "$_wb_status" -ne 0 ]; then
                # shellcheck disable=SC2154 # _wb_name is assigned via eval above
                echo "wasm_body: generator '$_wb_name' failed" >&2
                rm -f "$_wb_tmp"
                return 1
        fi

        wasm_var "$(wc -c <"$_wb_tmp")"
        cat "$_wb_tmp"
        rm -f "$_wb_tmp"
}

wasm_section() {
        section_id="$1"
        content_generator="$2"

        [ -z "$content_generator" ] && {
                echo "wasm_section: missing content generator" >&2
                return 1
        }

        bit_8 "$section_id"
        wasm_body "$content_generator"
}

wasm() {
        output="$1"
        code_generator="$2"

        if [ -z "$output" ] || [ -z "$code_generator" ]; then
                echo "wasm: usage: wasm <output> <generator>" >&2
                return 1
        fi

        code_section=$(bit_tempfile wasm) || {
                echo "wasm: could not create temp file" >&2
                return 1
        }

        if ! $code_generator >"$code_section"; then
                echo "wasm: code generator '$code_generator' failed" >&2
                rm -f "$code_section"
                return 1
        fi

        {
                bit_8 0x00, 0x61, 0x73, 0x6d
                bit_32 0x01
                cat "$code_section"
        } >"$output" || {
                echo "wasm: failed to write $output" >&2
                rm -f "$code_section"
                return 1
        }

        rm -f "$code_section"
}
