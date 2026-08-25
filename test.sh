#!/bin/bash
#
#       Test Kit
#       Testing binaries across multiple architectures using QEMU.
#
#       Dawn Larsson (dawning.dev) - 2022 - Apache License 2.0
#       repo: https://github.com/dawnlarsson/dawning-kit
#
# shellcheck disable=SC2034 # the full colour palette is part of the public surface

TIMEOUT="${TEST_TIMEOUT:-5}"

# coreutils' timeout is absent on macOS and is gtimeout when installed via
# brew. Without this every run reports exit 127 instead of the real result.
test_timeout_cmd() {
        if command -v timeout >/dev/null 2>&1; then
                echo timeout
        elif command -v gtimeout >/dev/null 2>&1; then
                echo gtimeout
        fi
}

TIMEOUT_CMD=$(test_timeout_cmd)

# Runs a command under the timeout tool when one exists, directly otherwise.
run_limited() {
        if [ -n "$TIMEOUT_CMD" ]; then
                "$TIMEOUT_CMD" "$TIMEOUT" "$@"
        else
                "$@"
        fi
}

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

# Each test runs in a background subshell, so a counter incremented here would
# never reach the parent. The verdict goes to a status file and the parent
# tallies the files after the wait.
test_architecture() {
        local arch_name="$1"
        local qemu_bin="$2"
        local gcc_cross="$3"
        local arch_desc="$4"
        local file_name="$5"
        local status_file="$6"

        local binary="$file_name.$arch_name"

        echo -e "\n${BOLD}${CYAN}$arch_name ${NC} $arch_desc"

        if [ -n "$qemu_bin" ] && ! command -v "$qemu_bin" >/dev/null 2>&1; then
                echo -e "${YELLOW}SKIPPED${NC} (QEMU not installed: $qemu_bin)"
                echo skip >"$status_file"
                return
        fi

        if [ ! -f "$binary" ]; then
                echo -e "${YELLOW}${BOLD}${INVERT}  FAIL  ${NC}${BOLD}  BINARY MISSING:  $binary ${NC}"
                echo fail >"$status_file"
                return
        fi

        # Whole seconds only: %N is a GNU extension that BSD date prints
        # literally, and bc is not always installed.
        local start_time exit_code=0
        start_time=$(date +%s)

        if [ -z "$qemu_bin" ]; then
                run_limited "./$binary"
                exit_code=$?
        else
                run_limited "$qemu_bin" "$binary"
                exit_code=$?
        fi

        local end_time
        end_time=$(date +%s)
        local duration=$((end_time - start_time))

        if [ $exit_code -eq 0 ]; then
                echo -e "${GREEN}${BOLD}${INVERT}  PASS  ${NC}${BOLD}  ${duration}s ${NC}"
                echo pass >"$status_file"
        else
                echo -e "${RED}${BOLD}${INVERT}  ERR   ${NC}  Exit: $exit_code  :  ${duration}s"
                echo fail >"$status_file"
        fi
}

# test_all <bin_directory> <binary_basename>
# Expects <bin_directory>/<basename>.<arch> for each architecture above.
# Returns non-zero when any architecture failed, so it can gate CI.
test_all() {
        local bin_dir="$1"
        local file_name="$2"

        if [ -z "$bin_dir" ] || [ -z "$file_name" ]; then
                echo "test_all: usage: test_all <bin_directory> <binary_basename>" >&2
                return 2
        fi

        PASSED=0
        FAILED=0
        SKIPPED=0
        TOTAL=0

        local work_dir
        work_dir=$(mktemp -d "${TMPDIR:-/tmp}/test.XXXXXXXX") || {
                echo "test_all: could not create temp directory" >&2
                return 2
        }

        local arch_config arch_name qemu_bin gcc_cross arch_desc

        for arch_config in "${ARCHITECTURES[@]}"; do
                IFS=':' read -r arch_name qemu_bin gcc_cross arch_desc <<<"$arch_config"

                test_architecture "$arch_name" "$qemu_bin" "$gcc_cross" "$arch_desc" \
                        "$bin_dir/$file_name" "$work_dir/$arch_name.status" \
                        >"$work_dir/$arch_name.out" 2>&1 &

                TOTAL=$((TOTAL + 1))
        done

        wait

        for arch_config in "${ARCHITECTURES[@]}"; do
                IFS=':' read -r arch_name qemu_bin gcc_cross arch_desc <<<"$arch_config"

                cat "$work_dir/$arch_name.out"

                case $(cat "$work_dir/$arch_name.status" 2>/dev/null) in
                pass) PASSED=$((PASSED + 1)) ;;
                skip) SKIPPED=$((SKIPPED + 1)) ;;
                *) FAILED=$((FAILED + 1)) ;;
                esac
        done

        rm -rf "$work_dir"

        echo
        echo -e "${BOLD}$PASSED passed, $FAILED failed, $SKIPPED skipped, $TOTAL total${NC}"

        [ "$FAILED" -eq 0 ]
}
