#!/bin/sh

#
#       Dawning Bit/kit Emit
#       cross architecture instruction emitters
#
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-devkit
#

# shellcheck disable=SC1091
. "${KIT_DIR:-./dawning-kit}/bit.sh"

ARCH=$(uname -m)

reg_0() {
        case "$ARCH" in
        x86_64) echo 0 ;;  # rax
        aarch64) echo 0 ;; # x0
        riscv64) echo 10 ;; # a0 (x10)
        esac
}

reg_1() {
        case "$ARCH" in
        x86_64) echo 1 ;;  # rcx
        aarch64) echo 1 ;; # x1
        riscv64) echo 11 ;; # a1 (x11)
        esac
}

reg_2() {
        case "$ARCH" in
        x86_64) echo 2 ;;  # rdx
        aarch64) echo 2 ;; # x2
        riscv64) echo 12 ;; # a2 (x12)
        esac
}

reg_3() {
        case "$ARCH" in
        x86_64) echo 3 ;;  # rbx
        aarch64) echo 3 ;; # x3
        riscv64) echo 13 ;; # a3 (x13)
        esac
}

reg_4() {
        case "$ARCH" in
        x86_64) echo 4 ;;  # rsp
        aarch64) echo 4 ;; # x4
        riscv64) echo 14 ;; # a4 (x14)
        esac
}

reg_5() {
        case "$ARCH" in
        x86_64) echo 5 ;;  # rbp
        aarch64) echo 5 ;; # x5
        riscv64) echo 15 ;; # a5 (x15)
        esac
}

reg_6() {
        case "$ARCH" in
        x86_64) echo 6 ;;  # rsi
        aarch64) echo 6 ;; # x6
        riscv64) echo 16 ;; # a6 (x16)
        esac
}

reg_7() {
        case "$ARCH" in
        x86_64) echo 7 ;;  # rdi
        aarch64) echo 7 ;; # x7
        riscv64) echo 17 ;; # a7 (x17)
        esac
}

reg_8() {
        case "$ARCH" in
        x86_64) echo 8 ;;  # r8
        aarch64) echo 8 ;; # x8
        riscv64) echo 8 ;; # s0/fp (x8)
        esac
}

reg_9() {
        case "$ARCH" in
        x86_64) echo 9 ;;  # r9
        aarch64) echo 9 ;; # x9
        riscv64) echo 9 ;; # s1 (x9)
        esac
}

reg_10() {
        case "$ARCH" in
        x86_64) echo 10 ;; # r10
        aarch64) echo 10 ;; # x10
        riscv64) echo 18 ;; # s2 (x18)
        esac
}

reg_11() {
        case "$ARCH" in
        x86_64) echo 11 ;; # r11
        aarch64) echo 11 ;; # x11
        riscv64) echo 19 ;; # s3 (x19)
        esac
}

reg_12() {
        case "$ARCH" in
        x86_64) echo 12 ;; # r12
        aarch64) echo 12 ;; # x12
        riscv64) echo 20 ;; # s4 (x20)
        esac
}

reg_13() {
        case "$ARCH" in
        x86_64) echo 13 ;; # r13
        aarch64) echo 13 ;; # x13
        riscv64) echo 21 ;; # s5 (x21)
        esac
}

reg_14() {
        case "$ARCH" in
        x86_64) echo 14 ;; # r14
        aarch64) echo 14 ;; # x14
        riscv64) echo 22 ;; # s6 (x22)
        esac
}

reg_15() {
        case "$ARCH" in
        x86_64) echo 15 ;; # r15
        aarch64) echo 15 ;; # x15
        riscv64) echo 23 ;; # s7 (x23)
        esac
}

reg_sp() {
        case "$ARCH" in
        x86_64) echo 4 ;;  # rsp
        aarch64) echo 31 ;; # sp
        riscv64) echo 2 ;;  # sp (x2)
        esac
}

reg_fp() {
        case "$ARCH" in
        x86_64) echo 5 ;;  # rbp
        aarch64) echo 29 ;; # x29
        riscv64) echo 8 ;;  # s0/fp (x8)
        esac
}

reg_lr() {
        case "$ARCH" in
        x86_64) echo 0 ;;  # no link register, use rax
        aarch64) echo 30 ;; # x30 (lr)
        riscv64) echo 1 ;;  # ra (x1)
        esac
}

reg_zero() {
        case "$ARCH" in
        x86_64) echo 0 ;;  # no zero register
        aarch64) echo 31 ;; # xzr/wzr
        riscv64) echo 0 ;;  # x0 (zero)
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

mov_reg() {
        dst="$1"
        src="$2"

        case "$ARCH" in
        x86_64)
                # mov dst, src: REX.W + 0x89 + ModR/M
                if [ "$dst" -ge 8 ] || [ "$src" -ge 8 ]; then
                        # Calculate REX byte
                        rex=0x48 # REX.W
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
                bit_8 0x89 # mov r/m64, r64
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
        esac
}