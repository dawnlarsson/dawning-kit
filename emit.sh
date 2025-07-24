#!/bin/sh

#
#       Dawning Bit/kit Emit
#       cross architecture instruction emitters
#
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-devkit
#

. bit.sh

ARCH=$(uname -m)

reg_0() {
        case "$ARCH" in
        x86_64) bit_8 0xc0 ;; # rax encoding
        aarch64) bit_8 0 ;;   # x0 encoding
        riscv64) bit_8 10 ;;  # a0 encoding
        esac
}

reg_1() {
        case "$ARCH" in
        x86_64) bit_8 3 ;;   # rbx encoding
        aarch64) bit_8 1 ;;  # x1 encoding
        riscv64) bit_8 11 ;; # a1 encoding
        esac
}

reg_2() {
        case "$ARCH" in
        x86_64) bit_8 1 ;;   # rcx encoding
        aarch64) bit_8 2 ;;  # x2 encoding
        riscv64) bit_8 12 ;; # a2 encoding
        esac
}

reg_3() {
        case "$ARCH" in
        x86_64) bit_8 2 ;;   # rdx encoding
        aarch64) bit_8 3 ;;  # x3 encoding
        riscv64) bit_8 13 ;; # a3 encoding
        esac
}

reg_4() {
        case "$ARCH" in
        x86_64) bit_8 4 ;;   # rsi encoding
        aarch64) bit_8 4 ;;  # x4 encoding
        riscv64) bit_8 14 ;; # a4 encoding
        esac
}

reg_5() {
        case "$ARCH" in
        x86_64) bit_8 0xc7 ;; # rdi encoding
        aarch64) bit_8 5 ;;   # x5 encoding
        riscv64) bit_8 15 ;;  # a5 encoding
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
                bit_32 0x00008067 # ret
                ;;
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
        esac
}

copy() {
        case "$ARCH" in
        x86_64)
                bit_8 0x48, 0xc7 # mov prefix
                ;;
        aarch64)
                bit_32 0xaa0003e0 # mov prefix
                ;;
        riscv64)
                bit_32 0x00000033 # mv prefix
                ;;
        esac
}

system_call() {

        syscall
        ret
}

asm_mov_imm() {
        reg_func="$1"
        value="$2"

        case "$ARCH" in
        x86_64)
                bit_8 0x48, 0xb8 # mov rax, imm64 prefix
                $reg_func        # emit register encoding
                bit_64 "$value"  # emit immediate value
                ;;
        aarch64)
                bit_32 0xd2800000 # mov immediate prefix
                $reg_func         # emit register encoding
                bit_16 "$value"   # emit immediate value
                ;;
        riscv64)
                bit_32 0x00000013 # li prefix
                $reg_func         # emit register encoding
                bit_32 "$value"   # emit immediate value
                ;;
        esac
}
