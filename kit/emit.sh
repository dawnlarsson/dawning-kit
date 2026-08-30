#!/bin/sh

#
#       Emit
#       cross architecture instruction emitters
#
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-kit
#

# This file is meant to be sourced, so $0 is the caller, not this script.
# BASH_SOURCE covers bash; the rest are the layouts people actually use.
kit_dir_find() {
        # shellcheck disable=SC3028 # BASH_SOURCE is probed on purpose; empty under dash
        for _c in "$KIT_DIR" "${BASH_SOURCE%/*}" "$(dirname -- "$0" 2>/dev/null)" . ./dawning-kit; do
                if [ -n "$_c" ] && [ -r "$_c/bit.sh" ]; then
                        printf '%s' "$_c"
                        return 0
                fi
        done
        return 1
}

if ! KIT_DIR=$(kit_dir_find); then
        echo "emit.sh: cannot locate bit.sh -- set KIT_DIR to the dawning-kit directory" >&2
        return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1091
. "$KIT_DIR/bit.sh" || {
        echo "emit.sh: failed to load $KIT_DIR/bit.sh" >&2
        return 1 2>/dev/null || exit 1
}

# uname -m is not consistent across systems: macOS reports arm64 where Linux
# reports aarch64, and amd64 appears on the BSDs. Normalize once, here.
emit_arch_normalize() {
        case "$1" in
        x86_64 | amd64 | x64) echo x86_64 ;;
        aarch64 | arm64) echo aarch64 ;;
        riscv64 | riscv) echo riscv64 ;;
        *) return 1 ;;
        esac
}

if ! ARCH=$(emit_arch_normalize "${ARCH:-$(uname -m)}"); then
        echo "emit.sh: unsupported architecture '$(uname -m)'" >&2
        echo "emit.sh: supported: x86_64, aarch64, riscv64 (override with ARCH=)" >&2
        return 1 2>/dev/null || exit 1
fi

# Every emitter ends in this so an unhandled arch is loud, never a silent
# zero byte output.
emit_unsupported() {
        echo "emit: '$1' is not implemented for $ARCH" >&2
        return 1
}

#
# The sixteen numbered helpers below were this same three-way mapping written
# out sixteen times. The rule, once:
#
#   x86_64:  0..15 are rax rcx rdx rbx rsp rbp rsi rdi r8..r15, in encoding
#            order, so the number is the answer.
#   aarch64: 0..15 are x0..x15.
#   riscv64: 0..7 land on the argument registers a0..a7 (x10..x17), 8..9 stay
#            s0/s1 (x8/x9), and 10..15 continue into s2..s7 (x18..x23).
#
reg() {
        case "$ARCH" in
        x86_64 | aarch64) echo "$1" ;;
        riscv64)
                if [ "$1" -lt 8 ]; then
                        echo "$(($1 + 10))"
                elif [ "$1" -lt 10 ]; then
                        echo "$1"
                else
                        echo "$(($1 + 8))"
                fi
                ;;
        *) emit_unsupported "reg_$1" ;;
        esac
}

reg_0() { reg 0; }
reg_1() { reg 1; }
reg_2() { reg 2; }
reg_3() { reg 3; }
reg_4() { reg 4; }
reg_5() { reg 5; }
reg_6() { reg 6; }
reg_7() { reg 7; }
reg_8() { reg 8; }
reg_9() { reg 9; }
reg_10() { reg 10; }
reg_11() { reg 11; }
reg_12() { reg 12; }
reg_13() { reg 13; }
reg_14() { reg 14; }
reg_15() { reg 15; }

reg_sp() {
        case "$ARCH" in
        x86_64) echo 4 ;;   # rsp
        aarch64) echo 31 ;; # sp
        riscv64) echo 2 ;;  # sp (x2)
        *) emit_unsupported reg_sp ;;
        esac
}

reg_fp() {
        case "$ARCH" in
        x86_64) echo 5 ;;   # rbp
        aarch64) echo 29 ;; # x29
        riscv64) echo 8 ;;  # s0/fp (x8)
        *) emit_unsupported reg_fp ;;
        esac
}

reg_lr() {
        case "$ARCH" in
        x86_64) echo 0 ;;   # no link register, use rax
        aarch64) echo 30 ;; # x30 (lr)
        riscv64) echo 1 ;;  # ra (x1)
        *) emit_unsupported reg_lr ;;
        esac
}

reg_zero() {
        case "$ARCH" in
        x86_64) echo 0 ;;   # no zero register
        aarch64) echo 31 ;; # xzr/wzr
        riscv64) echo 0 ;;  # x0 (zero)
        *) emit_unsupported reg_zero ;;
        esac
}

ret() {
        case "$ARCH" in
        x86_64)
                bit_8 0xc3 # ret
                ;;
        aarch64)
                bit_32 0xd65f03c0 # ret
                ;;
        riscv64)
                bit_32 0x00008067 # ret (jalr x0, x1, 0)
                ;;
        *) emit_unsupported ret ;;
        esac
}

syscall() {
        case "$ARCH" in
        x86_64)
                bit_8 0x0f, 0x05 # syscall
                ;;
        aarch64)
                bit_32 0xd4000001 # svc #0
                ;;
        riscv64)
                bit_32 0x00000073 # ecall
                ;;
        *) emit_unsupported syscall ;;
        esac
}

nop() {
        case "$ARCH" in
        x86_64)
                bit_8 0x90 # nop
                ;;
        aarch64)
                bit_32 0xd503201f # nop
                ;;
        riscv64)
                bit_32 0x00000013 # addi x0, x0, 0 (nop)
                ;;
        *) emit_unsupported nop ;;
        esac
}

system_call() {
        syscall && ret
}

# mov_imm <dst> <imm>
# Loads a small immediate (0..65535) into a register. Replaces the old copy(),
# which emitted a bare x86 prefix with no ModR/M or immediate -- not a valid
# instruction on any of these architectures.
mov_imm() {
        dst="$1"
        imm="$2"

        if [ -z "$dst" ] || [ -z "$imm" ]; then
                echo "mov_imm: usage: mov_imm <dst> <imm>" >&2
                return 1
        fi

        if [ "$imm" -lt 0 ] || [ "$imm" -gt 65535 ]; then
                echo "mov_imm: immediate $imm out of range (0..65535)" >&2
                return 1
        fi

        case "$ARCH" in
        x86_64)
                # mov r64, imm32: REX.W + 0xC7 /0 id
                if [ "$dst" -ge 8 ]; then
                        bit_8 $((0x48 | 0x01)) # REX.W + REX.B
                        bit_8 0xc7
                        bit_8 $((0xC0 | (dst % 8)))
                else
                        bit_8 0x48
                        bit_8 0xc7
                        bit_8 $((0xC0 | dst))
                fi
                bit_32 "$imm"
                ;;
        aarch64)
                # movz xd, #imm16
                bit_32 $((0xd2800000 | (imm << 5) | dst))
                ;;
        riscv64)
                # addi rd, x0, imm12 only reaches 0..2047; use lui+addi above that
                if [ "$imm" -le 2047 ]; then
                        bit_32 $((imm << 20 | 0 << 15 | 0 << 12 | dst << 7 | 0x13))
                else
                        # lui rd, imm[31:12] ; addi rd, rd, imm[11:0]
                        upper=$(((imm + 0x800) >> 12))
                        lower=$((imm - (upper << 12)))
                        bit_32 $((upper << 12 | dst << 7 | 0x37))
                        bit_32 $(((lower & 0xFFF) << 20 | dst << 15 | 0 << 12 | dst << 7 | 0x13))
                fi
                ;;
        *) emit_unsupported mov_imm ;;
        esac
}

mov_reg() {
        dst="$1"
        src="$2"

        if [ -z "$dst" ] || [ -z "$src" ]; then
                echo "mov_reg: usage: mov_reg <dst> <src>" >&2
                return 1
        fi

        case "$ARCH" in
        x86_64)
                # mov dst, src: REX.W + 0x89 + ModR/M
                if [ "$dst" -ge 8 ] || [ "$src" -ge 8 ]; then
                        # Calculate REX byte
                        rex=0x48                                # REX.W
                        [ "$dst" -ge 8 ] && rex=$((rex | 0x01)) # REX.B
                        [ "$src" -ge 8 ] && rex=$((rex | 0x04)) # REX.R
                        bit_8 "$rex"
                        dst_mod=$((dst % 8))
                        src_mod=$((src % 8))
                else
                        bit_8 0x48 # REX.W
                        dst_mod="$dst"
                        src_mod="$src"
                fi
                bit_8 0x89                                 # mov r/m64, r64
                bit_8 $((0xC0 | (src_mod << 3) | dst_mod)) # ModR/M
                ;;
        aarch64)
                # mov xd, xn: ORR xd, xzr, xn
                bit_32 $((0xaa0003e0 | (src << 16) | dst))
                ;;
        riscv64)
                # mv rd, rs: addi rd, rs, 0
                bit_32 $((0 << 20 | src << 15 | 0 << 12 | dst << 7 | 0x13))
                ;;
        *) emit_unsupported mov_reg ;;
        esac
}
