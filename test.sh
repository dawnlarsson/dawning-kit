#!/bin/bash
#
#       Dawning Test Kit
#       Testing binaries across multiple architectures using QEMU.
#
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-devkit
#

TOTAL=0
PASSED=0
TIMEOUT="${2:-5}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[1;36m'
NC='\033[0m'
BOLD='\033[1m'
INVERT='\033[7m'
BLINK='\033[4m'

ARCHITECTURES=(
        "x86_64:qemu-x86_64:x86_64-linux-gnu-gcc:64-bit x86 (AMD64)"
        "i386:qemu-i386:i386-linux-gnu-gcc:32-bit x86"
        "arm:qemu-arm:arm-linux-gnueabihf-gcc:32-bit ARM (EABI)"
        "aarch64:qemu-aarch64:aarch64-linux-gnu-gcc:64-bit ARM (ARMv8)"
        "mips:qemu-mips:mips-linux-gnu-gcc:32-bit MIPS (big-endian)"
        "mipsel:qemu-mipsel:mipsel-linux-gnu-gcc:32-bit MIPS (little-endian)"
        "mips64:qemu-mips64:mips64-linux-gnuabi64-gcc:64-bit MIPS (big-endian)"
        "mips64el:qemu-mips64el:mips64el-linux-gnuabi64-gcc:64-bit MIPS (little-endian)"
        "ppc:qemu-ppc:powerpc-linux-gnu-gcc:32-bit PowerPC"
        "ppc64:qemu-ppc64:powerpc64-linux-gnu-gcc:64-bit PowerPC (big-endian)"
        "ppc64le:qemu-ppc64le:powerpc64le-linux-gnu-gcc:64-bit PowerPC (little-endian)"
        "riscv32:qemu-riscv32:riscv32-linux-gnu-gcc:32-bit RISC-V"
        "riscv64:qemu-riscv64:riscv64-linux-gnu-gcc:64-bit RISC-V"
        "s390x:qemu-s390x:s390x-linux-gnu-gcc:IBM System z"
        "sparc:qemu-sparc:sparc-linux-gnu-gcc:32-bit SPARC"
        "sparc64:qemu-sparc64:sparc64-linux-gnu-gcc:64-bit SPARC"
        "alpha:qemu-alpha:alpha-linux-gnu-gcc:DEC Alpha"
        "sh4:qemu-sh4:sh4-linux-gnu-gcc:SuperH SH-4"
        "m68k:qemu-m68k:m68k-linux-gnu-gcc:Motorola 68000"
)

test_architecture() {
        local arch_name="$1"
        local qemu_bin="$2"
        local gcc_cross="$3"
        local arch_desc="$4"
        local file_name="$5"

        local binary="$file_name.$arch_name"

        echo -e "\n${BOLD}${CYAN}$arch_name ${NC} $arch_desc"

        if [ -n "$qemu_bin" ] && ! command -v "$qemu_bin" >/dev/null 2>&1; then
                echo -e "${YELLOW}SKIPPED${NC} (QEMU not installed: $qemu_bin)"
                return
        fi

        local start_time=$(date +%s.%N)
        local exit_code=0

        if [ ! -f "$binary" ]; then
                echo -e "${YELLOW}${BOLD}${INVERT}  FAIL  ${NC}${BOLD}  BINARY MISSING:  $binary ${NC}"
                return
        fi

        if [ -z "$qemu_bin" ]; then
                timeout "$TIMEOUT" "./$binary"
                exit_code=$?
        else
                timeout "$TIMEOUT" "$qemu_bin" "$binary"
                exit_code=$?
        fi

        local end_time=$(date +%s.%N)
        local duration=$(echo "$end_time - $start_time" | bc)

        if [ $exit_code -eq 0 ]; then
                echo -e "${GREEN}${BOLD}${INVERT}  PASS  ${NC}${BOLD}  ${duration}s ${NC}"
                ((PASSED++))
        else
                echo -e "${RED}${BOLD}${INVERT}  ERR   ${NC}  Exit: $exit_code  :  ${duration}s"
        fi
}

test_all() {
        PATH="$1"
        FILE_NAME="$2"

        PASSED=0
        TOTAL=0

        for arch_config in "${ARCHITECTURES[@]}"; do
                IFS=':' read -r arch_name qemu_bin gcc_cross arch_desc <<<"$arch_config"

                test_architecture "$arch_name" "$qemu_bin" "$gcc_cross" "$arch_desc" "$PATH/$FILE_NAME" >"/tmp/test_${arch_name}.out" 2>&1 &

                ((TOTAL++))
        done

        wait

        for arch_config in "${ARCHITECTURES[@]}"; do
                IFS=':' read -r arch_name qemu_bin gcc_cross arch_desc <<<"$arch_config"
                cat "/tmp/test_${arch_name}.out"
        done
}

# while inotifywait -q -e modify -e create -e delete -r .; do
#         run
# done
